#include "CudaPointFocus.hpp"

#include <cuda_runtime.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <thread>
#include <unordered_map>
#include <vector>

namespace cgh::solver::CudaPointFocus {
namespace {
using Clock = std::chrono::steady_clock;
constexpr double kCancellationTolerance = 32.0 * std::numeric_limits<double>::epsilon();
constexpr std::size_t kPixelsPerTile = 8192;
constexpr std::size_t kEmittersPerBatch = 256;
constexpr std::size_t kUploadEmittersPerBatch = 8192;
constexpr unsigned kThreadsPerBlock = 128;

struct Emitter {
    double x, y, z;
    double amplitude, phase;
    double weighted_real = 0, weighted_imaginary = 0;
};
struct Aperture {
    double half_y, half_z, k, minimum_incident, maximum_incident;
};
struct CompensatedSum {
    double sum = 0, correction = 0;
    __host__ __device__ void Add(double value) {
        const double next = sum + value;
        correction += ::fabs(sum) >= ::fabs(value) ? (sum - next) + value : (value - next) + sum;
        sum = next;
    }
    __host__ __device__ double Value() const { return sum + correction; }
};
struct PixelSums { CompensatedSum real, imaginary; };
struct Grid {
    std::uint32_t width, height;
    double pitch_x, pitch_y, k, initial_phase, direction_y, direction_z, zero_threshold;
};

bool CheckCancelled(const std::atomic<bool>& cancelled, std::string& error) {
    return !cancelled.load(std::memory_order_relaxed) || wire::Fail(error, "PointFocus job cancelled.");
}

// Scale only extreme arguments. In the usual optical range this preserves the
// x*x + y*y + z*z order used by UE's libc++ std::hypot(x,y,z). CUDA and host libm
// need not be bit-identical; the algorithm and FP64 representation are shared.
__host__ __device__ double Distance(double x, double y, double z) {
    const double maximum = ::fmax(::fabs(x), ::fmax(::fabs(y), ::fabs(z)));
    constexpr double threshold = 0x1p512;
    double scale = 1.0;
    if (maximum > threshold) scale = 0x1p-532;
    else if (maximum < 1.0 / threshold) scale = 0x1p532;
    x *= scale;
    y *= scale;
    z *= scale;
    return ::sqrt((x * x + y * y) + z * z) / scale;
}

wire::Vec3 Rotate(const wire::Quat& q, const wire::Vec3& v) {
    // Same evaluation as FQuat::RotateVector: T=2*(Q cross V), V+w*T+(Q cross T).
    const wire::Vec3 t{2.0 * (q.y * v.z - q.z * v.y),
                       2.0 * (q.z * v.x - q.x * v.z),
                       2.0 * (q.x * v.y - q.y * v.x)};
    return {(v.x + q.w * t.x) + (q.y * t.z - q.z * t.y),
            (v.y + q.w * t.y) + (q.z * t.x - q.x * t.z),
            (v.z + q.w * t.z) + (q.x * t.y - q.y * t.x)};
}

bool ValidateEmitter(const wire::Vec3& position, double amplitude, double phase,
                     const Aperture& aperture, std::string& error) {
    if (!wire::IsFinite(position) || !std::isfinite(phase) || !std::isfinite(amplitude) || amplitude < 0)
        return wire::Fail(error, "Target/sample positions and phases must be finite; amplitudes must be finite and nonnegative.");
    if (amplitude == 0) return true;
    if (position.x == 0)
        return wire::Fail(error, "Every contributing point must lie off the SLM plane (SLM-local X must be nonzero).");
    const double maximum_distance = Distance(position.x, std::abs(position.y) + aperture.half_y,
                                             std::abs(position.z) + aperture.half_z);
    const double maximum_propagation_phase = aperture.k * maximum_distance;
    const double minimum_phase = phase - maximum_propagation_phase;
    if (!std::isfinite(maximum_distance) || !std::isfinite(maximum_propagation_phase) ||
        !std::isfinite(minimum_phase) || !std::isfinite(minimum_phase - aperture.maximum_incident) ||
        !std::isfinite(phase - aperture.minimum_incident))
        return wire::Fail(error, "The scene geometry, propagation phase or incident plane-wave phase exceeds finite double-precision bounds.");
    return true;
}

bool GatherEmitters(const wire::Request& request, std::vector<Emitter>& emitters,
                    Aperture& aperture, double& zero_threshold,
                    std::string& error, const std::atomic<bool>& cancelled) {
    if (!CheckCancelled(cancelled, error) || !wire::ValidateRequest(request, error, &cancelled)) return false;
    const auto& slm = request.slm;
    const auto& light = request.light;
    if (slm.modulation != wire::Modulation::PhaseOnly)
        return wire::Fail(error, "PointFocus requires a phase-only SLM.");
    if (light.source != wire::Source::PlaneWave)
        return wire::Fail(error, "PointFocus supports only PlaneWave reconstruction illumination.");
    aperture.k = wire::kTwoPi / light.wavelength_m;
    if (!std::isfinite(aperture.k))
        return wire::Fail(error, "The reconstruction wavelength must be finite and positive with a finite wave number.");
    const auto& direction = light.direction_slm;
    const double direction_norm_squared = (direction.x * direction.x + direction.y * direction.y) + direction.z * direction.z;
    if (!std::isfinite(direction_norm_squared) || std::abs(direction_norm_squared - 1.0) > 1.0e-6)
        return wire::Fail(error, "Plane-wave DirectionSLM must be finite and unit length (squared-norm tolerance 1e-6).");
    aperture.half_y = (slm.resolution_x - 1) / 2.0 * slm.pixel_pitch_x_m;
    aperture.half_z = (slm.resolution_y - 1) / 2.0 * slm.pixel_pitch_y_m;
    const double maximum_incident_spatial_phase = aperture.k *
        (std::abs(direction.y) * aperture.half_y + std::abs(direction.z) * aperture.half_z);
    aperture.minimum_incident = light.initial_phase_rad - maximum_incident_spatial_phase;
    aperture.maximum_incident = light.initial_phase_rad + maximum_incident_spatial_phase;
    if (!std::isfinite(aperture.half_y) || !std::isfinite(aperture.half_z) ||
        !std::isfinite(maximum_incident_spatial_phase) || !std::isfinite(aperture.minimum_incident) ||
        !std::isfinite(aperture.maximum_incident))
        return wire::Fail(error, "The SLM aperture or incident plane-wave phase exceeds finite double-precision bounds.");

    std::unordered_map<std::uint64_t, const wire::PointCloud*> clouds;
    std::size_t total_points = 0;
    for (const auto& cloud : request.point_clouds) {
        if (!CheckCancelled(cancelled, error)) return false;
        clouds.emplace(cloud.resource_id, &cloud);
        total_points += cloud.points.size();
    }
    for (const auto& target : request.targets) {
        if (!CheckCancelled(cancelled, error)) return false;
        if (target.kind == wire::TargetKind::Point) {
            ++total_points;
            if (!ValidateEmitter(target.position_slm_m, target.amplitude, target.phase_rad, aperture, error)) return false;
        } else {
            const auto& q = target.rotation_slm;
            const double norm_squared = ((q.x * q.x + q.y * q.y) + q.z * q.z) + q.w * q.w;
            if (!std::isfinite(norm_squared) || std::abs(norm_squared - 1.0) > 1.0e-6)
                return wire::Fail(error, "Mesh RotationSLM must be finite and unit length (squared-norm tolerance 1e-6).");
        }
    }
    if (total_points > wire::kMaxEmitters)
        return wire::Fail(error, "PointFocus supports at most 1,000,000 total source points.");
    emitters.reserve(total_points);
    double maximum_amplitude = 0;
    const auto add = [&emitters, &maximum_amplitude](const wire::Vec3& position, double amplitude, double phase) {
        if (amplitude > 0) {
            maximum_amplitude = std::max(maximum_amplitude, amplitude);
            emitters.push_back({position.x, position.y, position.z, amplitude, phase});
        }
    };
    for (const auto& target : request.targets) {
        if (!CheckCancelled(cancelled, error)) return false;
        if (target.kind == wire::TargetKind::Point) {
            add(target.position_slm_m, target.amplitude, target.phase_rad);
            continue;
        }
        const auto found = clouds.find(target.resource_id);
        if (found == clouds.end() || found->second->revision != target.revision)
            return wire::Fail(error, "Each mesh target requires a point cloud with matching ResourceId and Revision.");
        for (const auto& sample : found->second->points) {
            if (!CheckCancelled(cancelled, error)) return false;
            const auto rotated = Rotate(target.rotation_slm, sample.position_local_m);
            const wire::Vec3 position{target.position_slm_m.x + rotated.x,
                                      target.position_slm_m.y + rotated.y,
                                      target.position_slm_m.z + rotated.z};
            if (!ValidateEmitter(position, sample.amplitude, sample.phase, aperture, error)) return false;
            // Sampling already baked target amplitude, phase and scale into this resource.
            add(position, sample.amplitude, sample.phase);
        }
    }
    if (!(maximum_amplitude > 0))
        return wire::Fail(error, "PointFocus requires at least one source point with positive amplitude.");
    CompensatedSum weight_sum;
    for (auto& emitter : emitters) {
        if (!CheckCancelled(cancelled, error)) return false;
        emitter.amplitude /= maximum_amplitude;
        emitter.weighted_real = emitter.amplitude * std::cos(emitter.phase);
        emitter.weighted_imaginary = emitter.amplitude * std::sin(emitter.phase);
        weight_sum.Add(emitter.amplitude);
    }
    zero_threshold = kCancellationTolerance * weight_sum.Value();
    return true;
}

bool CheckCuda(cudaError_t code, const char* operation, std::string& error) {
    if (code == cudaSuccess) return true;
    error = std::string("CUDA ") + operation + " failed: " + cudaGetErrorString(code);
    return false;
}

struct DeviceResources {
    cudaStream_t stream = nullptr;
    Emitter* emitters = nullptr;
    PixelSums* sums = nullptr;
    double* phases = nullptr;
    int* numerical_error = nullptr;
    bool Release(std::string* error = nullptr) {
        bool succeeded = true;
        const auto release = [&succeeded, error](cudaError_t code, const char* operation) {
            if (code != cudaSuccess) {
                if (succeeded && error) CheckCuda(code, operation, *error);
                succeeded = false;
            }
        };
        // No invocation queues more than one bounded kernel at a time. Draining
        // before freeing keeps cancellation safe without touching another job.
        if (stream) release(cudaStreamSynchronize(stream), "stream cleanup");
        if (numerical_error) { release(cudaFree(numerical_error), "error-buffer free"); numerical_error = nullptr; }
        if (phases) { release(cudaFree(phases), "phase-buffer free"); phases = nullptr; }
        if (sums) { release(cudaFree(sums), "sum-buffer free"); sums = nullptr; }
        if (emitters) { release(cudaFree(emitters), "emitter-buffer free"); emitters = nullptr; }
        if (stream) { release(cudaStreamDestroy(stream), "stream destroy"); stream = nullptr; }
        return succeeded;
    }
    ~DeviceResources() { Release(); }
};

bool WaitForStream(cudaStream_t stream, const std::atomic<bool>& cancelled, std::string& error) {
    bool cancellation_seen = false;
    for (;;) {
        cancellation_seen |= cancelled.load(std::memory_order_relaxed);
        const auto status = cudaStreamQuery(stream);
        if (status == cudaSuccess) return !cancellation_seen || wire::Fail(error, "PointFocus job cancelled.");
        if (status != cudaErrorNotReady) return CheckCuda(status, "stream completion", error);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

__device__ double WrapToTwoPi(double phase) {
    double wrapped = ::fmod(phase, wire::kTwoPi);
    if (wrapped < 0) wrapped += wire::kTwoPi;
    return wrapped >= wire::kTwoPi || wrapped == 0 ? 0 : wrapped;
}

__device__ void PixelPosition(std::size_t index, const Grid& grid, double& y, double& z) {
    const auto column = index % grid.width;
    const auto row = index / grid.width;
    y = (static_cast<double>(column) - (grid.width - 1) / 2.0) * grid.pitch_x;
    z = ((grid.height - 1) / 2.0 - static_cast<double>(row)) * grid.pitch_y;
}

__device__ void StorePhase(double phase, std::size_t index, double* phases, int* error) {
    if (!::isfinite(phase)) {
        atomicCAS(error, 0, 2);
        return;
    }
    phases[index] = WrapToTwoPi(phase);
}

__global__ void SingleEmitterKernel(const Emitter* emitters, Grid grid,
                                    std::size_t pixel_begin, std::size_t pixel_count,
                                    double* phases, int* error) {
    const std::size_t local = std::size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (local >= pixel_count) return;
    double yp, zp;
    PixelPosition(pixel_begin + local, grid, yp, zp);
    const auto& emitter = emitters[0];
    const double incident = grid.initial_phase + grid.k * (grid.direction_y * yp + grid.direction_z * zp);
    const double r = Distance(emitter.x, emitter.y - yp, emitter.z - zp);
    // Preserve the reference's analytic branch when only one positive source remains.
    StorePhase((emitter.phase - grid.k * r) - incident, local, phases, error);
}

__global__ void AccumulateKernel(const Emitter* emitters, std::size_t emitter_begin,
                                 std::size_t emitter_count, Grid grid,
                                 std::size_t pixel_begin, std::size_t pixel_count,
                                 PixelSums* states, bool final_batch,
                                 double* phases, int* error) {
    const std::size_t local = std::size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (local >= pixel_count) return;
    double yp, zp;
    PixelPosition(pixel_begin + local, grid, yp, zp);
    PixelSums state = states[local];
    // Each pixel visits sources in exactly target/sample order. State includes
    // both Neumaier corrections across batches; no parallel emitter reduction.
    for (std::size_t index = emitter_begin; index < emitter_begin + emitter_count; ++index) {
        const auto& emitter = emitters[index];
        const double propagation = grid.k * Distance(emitter.x, emitter.y - yp, emitter.z - zp);
        const double c = ::cos(propagation);
        const double s = ::sin(propagation);
        state.real.Add(emitter.weighted_real * c + emitter.weighted_imaginary * s);
        state.imaginary.Add(emitter.weighted_imaginary * c - emitter.weighted_real * s);
    }
    states[local] = state;
    if (!final_batch) return;
    const double field_real = state.real.Value();
    const double field_imaginary = state.imaginary.Value();
    if (!::isfinite(field_real) || !::isfinite(field_imaginary)) {
        atomicCAS(error, 0, 1);
        return;
    }
    const double incident = grid.initial_phase + grid.k * (grid.direction_y * yp + grid.direction_z * zp);
    const double phase = ::hypot(field_real, field_imaginary) <= grid.zero_threshold
        ? 0 : ::atan2(field_imaginary, field_real) - incident;
    StorePhase(phase, local, phases, error);
}

bool SolveInternal(const wire::Request& request, wire::Result& result,
                   std::string& error, const std::atomic<bool>& cancelled) {
    const auto start = Clock::now();
    std::vector<Emitter> emitters;
    Aperture aperture{};
    double zero_threshold = 0;
    if (!GatherEmitters(request, emitters, aperture, zero_threshold, error, cancelled)) return false;
    if (!CheckCancelled(cancelled, error)) return false;
    // Docker selects the physical GPU. The only visible V100 has logical ordinal 0.
    if (!CheckCuda(cudaSetDevice(0), "select logical device 0", error)) return false;
    DeviceResources device;
    if (!CheckCuda(cudaStreamCreateWithFlags(&device.stream, cudaStreamNonBlocking), "stream creation", error) ||
        !CheckCuda(cudaMalloc(reinterpret_cast<void**>(&device.emitters), emitters.size() * sizeof(Emitter)), "emitter allocation", error) ||
        !CheckCuda(cudaMalloc(reinterpret_cast<void**>(&device.sums), kPixelsPerTile * sizeof(PixelSums)), "sum allocation", error) ||
        !CheckCuda(cudaMalloc(reinterpret_cast<void**>(&device.phases), kPixelsPerTile * sizeof(double)), "phase allocation", error) ||
        !CheckCuda(cudaMalloc(reinterpret_cast<void**>(&device.numerical_error), sizeof(int)), "error allocation", error)) return false;
    if (!CheckCancelled(cancelled, error)) return false;
    for (std::size_t begin = 0; begin < emitters.size(); begin += kUploadEmittersPerBatch) {
        if (!CheckCancelled(cancelled, error)) return false;
        const auto count = std::min(kUploadEmittersPerBatch, emitters.size() - begin);
        if (!CheckCuda(cudaMemcpyAsync(device.emitters + begin, emitters.data() + begin, count * sizeof(Emitter),
                                      cudaMemcpyHostToDevice, device.stream), "emitter upload", error) ||
            !WaitForStream(device.stream, cancelled, error)) return false;
    }
    wire::Result pending;
    pending.status = wire::Status::PointFocusSuccess;
    pending.convention = request.convention;
    pending.resolution_x = request.slm.resolution_x;
    pending.resolution_y = request.slm.resolution_y;
    const std::size_t total_pixels = std::size_t(pending.resolution_x) * pending.resolution_y;
    pending.phase_radians.resize(total_pixels);
    const Grid grid{pending.resolution_x, pending.resolution_y,
                    request.slm.pixel_pitch_x_m, request.slm.pixel_pitch_y_m, aperture.k,
                    request.light.initial_phase_rad, request.light.direction_slm.y,
                    request.light.direction_slm.z, zero_threshold};
    for (std::size_t begin = 0; begin < total_pixels; begin += kPixelsPerTile) {
        if (!CheckCancelled(cancelled, error)) return false;
        const auto count = std::min(kPixelsPerTile, total_pixels - begin);
        const auto blocks = static_cast<unsigned>((count + kThreadsPerBlock - 1) / kThreadsPerBlock);
        if (!CheckCuda(cudaMemsetAsync(device.numerical_error, 0, sizeof(int), device.stream), "clear numerical error", error)) return false;
        if (emitters.size() == 1) {
            SingleEmitterKernel<<<blocks, kThreadsPerBlock, 0, device.stream>>>(device.emitters, grid, begin, count,
                                                                              device.phases, device.numerical_error);
            if (!CheckCuda(cudaGetLastError(), "single-emitter kernel launch", error) ||
                !WaitForStream(device.stream, cancelled, error)) return false;
        } else {
            if (!CheckCuda(cudaMemsetAsync(device.sums, 0, count * sizeof(PixelSums), device.stream), "clear compensated sums", error)) return false;
            for (std::size_t first = 0; first < emitters.size(); first += kEmittersPerBatch) {
                if (!CheckCancelled(cancelled, error)) return false;
                const auto emitter_count = std::min(kEmittersPerBatch, emitters.size() - first);
                const bool final = first + emitter_count == emitters.size();
                AccumulateKernel<<<blocks, kThreadsPerBlock, 0, device.stream>>>(device.emitters, first, emitter_count,
                    grid, begin, count, device.sums, final, device.phases, device.numerical_error);
                if (!CheckCuda(cudaGetLastError(), "coherent-sum kernel launch", error) ||
                    !WaitForStream(device.stream, cancelled, error)) return false;
            }
        }
        int numerical_error = 0;
        if (!CheckCuda(cudaMemcpyAsync(&numerical_error, device.numerical_error, sizeof(int), cudaMemcpyDeviceToHost,
                                      device.stream), "numerical-error download", error) ||
            !CheckCuda(cudaMemcpyAsync(pending.phase_radians.data() + begin, device.phases, count * sizeof(double),
                                      cudaMemcpyDeviceToHost, device.stream), "phase download", error) ||
            !WaitForStream(device.stream, cancelled, error)) return false;
        if (numerical_error == 1) return wire::Fail(error, "PointFocus encountered a nonfinite complex field.");
        if (numerical_error != 0) return wire::Fail(error, "PointFocus encountered a nonfinite propagation phase.");
    }
    if (!CheckCancelled(cancelled, error) || !device.Release(&error)) return false;
    pending.compute_seconds = std::chrono::duration<double>(Clock::now() - start).count();
    if (!wire::ValidateResult(pending, error, &cancelled)) return false;
    result = std::move(pending);
    return true;
}
} // namespace

bool Solve(const wire::Request& request, wire::Result& result,
           std::string& error, const std::atomic<bool>& cancelled) {
    result = {};
    error.clear();
    try {
        return SolveInternal(request, result, error, cancelled);
    } catch (const std::exception& exception) {
        error = std::string("PointFocus host allocation failed: ") + exception.what();
        return false;
    }
}
} // namespace cgh::solver::CudaPointFocus
