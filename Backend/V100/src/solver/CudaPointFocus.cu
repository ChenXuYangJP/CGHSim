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
// A full tile launches 512 blocks at 128 threads, supplying multiple blocks
// per V100 SM while retaining bounded cancellation between emitter batches.
constexpr std::size_t kPixelsPerTile = 65536;
constexpr std::size_t kEmittersPerBatch = 256;
constexpr std::size_t kUploadEmittersPerBatch = 8192;
constexpr unsigned kThreadsPerBlock = 128;

struct Emitter {
    double x, y, z;
    double amplitude, phase;
    double weighted_real = 0, weighted_imaginary = 0;
    int amplitude_exponent = 0; // Inverse-r mode stores raw amplitude as mantissa/exponent.
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
struct InverseRPixelSums {
    CompensatedSum real, imaginary, weight;
    int max_weight_exponent = 0;
    __device__ void Rescale(double factor) {
        real.sum *= factor;
        real.correction *= factor;
        imaginary.sum *= factor;
        imaginary.correction *= factor;
        weight.sum *= factor;
        weight.correction *= factor;
    }
};
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
    if (request.algorithm == wire::Algorithm::PointFocusInverseR) {
        // Keep raw amplitude's exponent before any normalization. A tiny source
        // close to a pixel can matter as much as a much brighter distant source.
        for (auto& emitter : emitters) {
            if (!CheckCancelled(cancelled, error)) return false;
            emitter.amplitude = std::frexp(emitter.amplitude, &emitter.amplitude_exponent);
            emitter.weighted_real = std::cos(emitter.phase);
            emitter.weighted_imaginary = std::sin(emitter.phase);
        }
        zero_threshold = 0; // This mode derives the threshold separately at each pixel.
        return true;
    }
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
    InverseRPixelSums* inverse_sums = nullptr;
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
        if (inverse_sums) { release(cudaFree(inverse_sums), "inverse-distance sum-buffer free"); inverse_sums = nullptr; }
        if (sums) { release(cudaFree(sums), "sum-buffer free"); sums = nullptr; }
        if (emitters) { release(cudaFree(emitters), "emitter-buffer free"); emitters = nullptr; }
        if (stream) { release(cudaStreamDestroy(stream), "stream destroy"); stream = nullptr; }
        return succeeded;
    }
    ~DeviceResources() { Release(); }
};

struct Cancellation {
    const std::atomic<bool>& requested;
    std::atomic<bool> stopped{false};

    bool IsStopped() const {
        return requested.load(std::memory_order_relaxed) || stopped.load(std::memory_order_acquire);
    }
    bool Check(std::string& error) const {
        return !IsStopped() || wire::Fail(error, "PointFocus job cancelled.");
    }
};

bool WaitForStream(cudaStream_t stream, const Cancellation& cancellation, std::string& error) {
    bool cancellation_seen = false;
    for (;;) {
        cancellation_seen |= cancellation.IsStopped();
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

template <bool InverseR>
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
    if constexpr (InverseR) {
        if (!::isfinite(r) || r <= 0) {
            atomicCAS(error, 0, 3);
            return;
        }
    }
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

__global__ void AccumulateInverseRKernel(const Emitter* emitters, std::size_t emitter_begin,
                                         std::size_t emitter_count, Grid grid,
                                         std::size_t pixel_begin, std::size_t pixel_count,
                                         InverseRPixelSums* states, bool final_batch,
                                         double* phases, int* error) {
    const std::size_t local = std::size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (local >= pixel_count) return;
    double yp, zp;
    PixelPosition(pixel_begin + local, grid, yp, zp);
    InverseRPixelSums state = states[local];
    for (std::size_t index = emitter_begin; index < emitter_begin + emitter_count; ++index) {
        const auto& emitter = emitters[index];
        const double r = Distance(emitter.x, emitter.y - yp, emitter.z - zp);
        if (!::isfinite(r) || r <= 0) {
            atomicCAS(error, 0, 3);
            return;
        }
        int distance_exponent;
        const double distance_mantissa = ::frexp(r, &distance_exponent);
        const int exponent = emitter.amplitude_exponent - distance_exponent;
        if (index == 0) state.max_weight_exponent = exponent;
        else if (exponent > state.max_weight_exponent) {
            state.Rescale(::ldexp(1.0, state.max_weight_exponent - exponent));
            state.max_weight_exponent = exponent;
        }
        const double weight = ::ldexp(emitter.amplitude / distance_mantissa,
                                      exponent - state.max_weight_exponent);
        const double propagation = grid.k * r;
        const double c = ::cos(propagation);
        const double s = ::sin(propagation);
        state.real.Add(weight * (emitter.weighted_real * c + emitter.weighted_imaginary * s));
        state.imaginary.Add(weight * (emitter.weighted_imaginary * c - emitter.weighted_real * s));
        state.weight.Add(weight);
    }
    states[local] = state;
    if (!final_batch) return;
    const double field_real = state.real.Value();
    const double field_imaginary = state.imaginary.Value();
    const double weight_sum = state.weight.Value();
    if (!::isfinite(field_real) || !::isfinite(field_imaginary) || !::isfinite(weight_sum)) {
        atomicCAS(error, 0, 1);
        return;
    }
    const double incident = grid.initial_phase + grid.k * (grid.direction_y * yp + grid.direction_z * zp);
    const double phase = ::hypot(field_real, field_imaginary) <= kCancellationTolerance * weight_sum
        ? 0 : ::atan2(field_imaginary, field_real) - incident;
    StorePhase(phase, local, phases, error);
}

// Every worker owns its CUDA context selection and resources. Pixel ranges are
// disjoint, but kernel offsets always refer to the original full SLM grid.
bool SolveRange(int ordinal, std::size_t pixel_begin, std::size_t pixel_end,
                const std::vector<Emitter>& emitters, const Grid& grid, bool inverse_r,
                std::vector<double>& phases, const Cancellation& cancellation,
                std::string& error) {
    if (!cancellation.Check(error) || !CheckCuda(cudaSetDevice(ordinal), "device selection", error)) return false;
    DeviceResources device;
    void** sums_address = inverse_r ? reinterpret_cast<void**>(&device.inverse_sums)
                                    : reinterpret_cast<void**>(&device.sums);
    const auto sums_bytes = kPixelsPerTile * (inverse_r ? sizeof(InverseRPixelSums) : sizeof(PixelSums));
    if (!CheckCuda(cudaStreamCreateWithFlags(&device.stream, cudaStreamNonBlocking), "stream creation", error) ||
        !CheckCuda(cudaMalloc(reinterpret_cast<void**>(&device.emitters), emitters.size() * sizeof(Emitter)), "emitter allocation", error) ||
        !CheckCuda(cudaMalloc(sums_address, sums_bytes), "sum allocation", error) ||
        !CheckCuda(cudaMalloc(reinterpret_cast<void**>(&device.phases), kPixelsPerTile * sizeof(double)), "phase allocation", error) ||
        !CheckCuda(cudaMalloc(reinterpret_cast<void**>(&device.numerical_error), sizeof(int)), "error allocation", error)) return false;
    if (!cancellation.Check(error)) return false;
    for (std::size_t begin = 0; begin < emitters.size(); begin += kUploadEmittersPerBatch) {
        if (!cancellation.Check(error)) return false;
        const auto count = std::min(kUploadEmittersPerBatch, emitters.size() - begin);
        if (!CheckCuda(cudaMemcpyAsync(device.emitters + begin, emitters.data() + begin, count * sizeof(Emitter),
                                      cudaMemcpyHostToDevice, device.stream), "emitter upload", error) ||
            !WaitForStream(device.stream, cancellation, error)) return false;
    }
    for (std::size_t begin = pixel_begin; begin < pixel_end; begin += kPixelsPerTile) {
        if (!cancellation.Check(error)) return false;
        const auto count = std::min(kPixelsPerTile, pixel_end - begin);
        const auto blocks = static_cast<unsigned>((count + kThreadsPerBlock - 1) / kThreadsPerBlock);
        if (!CheckCuda(cudaMemsetAsync(device.numerical_error, 0, sizeof(int), device.stream), "clear numerical error", error)) return false;
        if (emitters.size() == 1) {
            if (inverse_r) {
                SingleEmitterKernel<true><<<blocks, kThreadsPerBlock, 0, device.stream>>>(device.emitters, grid, begin, count,
                                                                                       device.phases, device.numerical_error);
            } else {
                SingleEmitterKernel<false><<<blocks, kThreadsPerBlock, 0, device.stream>>>(device.emitters, grid, begin, count,
                                                                                        device.phases, device.numerical_error);
            }
            if (!CheckCuda(cudaGetLastError(), "single-emitter kernel launch", error) ||
                !WaitForStream(device.stream, cancellation, error)) return false;
        } else {
            void* sums = inverse_r ? static_cast<void*>(device.inverse_sums) : static_cast<void*>(device.sums);
            const auto state_size = inverse_r ? sizeof(InverseRPixelSums) : sizeof(PixelSums);
            if (!CheckCuda(cudaMemsetAsync(sums, 0, count * state_size, device.stream), "clear compensated sums", error)) return false;
            for (std::size_t first = 0; first < emitters.size(); first += kEmittersPerBatch) {
                if (!cancellation.Check(error)) return false;
                const auto emitter_count = std::min(kEmittersPerBatch, emitters.size() - first);
                const bool final = first + emitter_count == emitters.size();
                if (inverse_r) {
                    AccumulateInverseRKernel<<<blocks, kThreadsPerBlock, 0, device.stream>>>(device.emitters, first, emitter_count,
                        grid, begin, count, device.inverse_sums, final, device.phases, device.numerical_error);
                } else {
                    AccumulateKernel<<<blocks, kThreadsPerBlock, 0, device.stream>>>(device.emitters, first, emitter_count,
                        grid, begin, count, device.sums, final, device.phases, device.numerical_error);
                }
                if (!CheckCuda(cudaGetLastError(), "coherent-sum kernel launch", error) ||
                    !WaitForStream(device.stream, cancellation, error)) return false;
            }
        }
        int numerical_error = 0;
        if (!CheckCuda(cudaMemcpyAsync(&numerical_error, device.numerical_error, sizeof(int), cudaMemcpyDeviceToHost,
                                      device.stream), "numerical-error download", error) ||
            !CheckCuda(cudaMemcpyAsync(phases.data() + begin, device.phases, count * sizeof(double),
                                      cudaMemcpyDeviceToHost, device.stream), "phase download", error) ||
            !WaitForStream(device.stream, cancellation, error)) return false;
        if (numerical_error == 3) return wire::Fail(error, "Inverse-distance PointFocus requires finite nonzero source distances.");
        if (numerical_error == 1) return wire::Fail(error, "PointFocus encountered a nonfinite complex field.");
        if (numerical_error != 0) return wire::Fail(error, "PointFocus encountered a nonfinite propagation phase.");
    }
    return cancellation.Check(error) && device.Release(&error);
}

struct WorkerOutcome {
    bool succeeded = false;
    std::string error;
    std::exception_ptr exception;
};

// In particular, stop and join already-started workers if starting another host
// thread throws. No worker may outlive the emitters or the pending result.
struct WorkerGroup {
    Cancellation& cancellation;
    std::vector<std::thread> threads;
    void Join() {
        for (auto& thread : threads) if (thread.joinable()) thread.join();
    }
    ~WorkerGroup() {
        cancellation.stopped.store(true, std::memory_order_release);
        Join();
    }
};

std::string ExceptionMessage(const std::exception_ptr& exception) {
    try {
        std::rethrow_exception(exception);
    } catch (const std::exception& failure) {
        return failure.what();
    } catch (...) {
        return "Unknown host exception.";
    }
}

bool SolveInternal(const wire::Request& request, wire::Result& result,
                   std::string& error, const std::atomic<bool>& cancelled) {
    const auto start = Clock::now();
    std::vector<Emitter> emitters;
    Aperture aperture{};
    double zero_threshold = 0;
    if (!GatherEmitters(request, emitters, aperture, zero_threshold, error, cancelled)) return false;
    if (!CheckCancelled(cancelled, error)) return false;
    int device_count = 0;
    if (!CheckCuda(cudaGetDeviceCount(&device_count), "device enumeration", error)) return false;
    if (device_count <= 0) return wire::Fail(error, "PointFocus requires at least one visible CUDA device.");

    const bool inverse_r = request.algorithm == wire::Algorithm::PointFocusInverseR;
    wire::Result pending;
    pending.status = inverse_r ? wire::Status::PointFocusInverseRSuccess : wire::Status::PointFocusSuccess;
    pending.convention = request.convention;
    pending.resolution_x = request.slm.resolution_x;
    pending.resolution_y = request.slm.resolution_y;
    const std::size_t total_pixels = std::size_t(pending.resolution_x) * pending.resolution_y;
    pending.phase_radians.resize(total_pixels);
    const Grid grid{pending.resolution_x, pending.resolution_y,
                    request.slm.pixel_pitch_x_m, request.slm.pixel_pitch_y_m, aperture.k,
                    request.light.initial_phase_rad, request.light.direction_slm.y,
                    request.light.direction_slm.z, zero_threshold};

    // Docker selects physical GPUs; CUDA exposes them as logical devices 0..N-1.
    // Splitting only pixels leaves every source and compensated sum in precisely
    // the same order as a single-device solve, without any cross-device reduction.
    const auto worker_count = std::min(total_pixels, static_cast<std::size_t>(device_count));
    const auto pixels_per_device = total_pixels / worker_count;
    const auto remainder = total_pixels % worker_count;
    std::vector<WorkerOutcome> outcomes(worker_count);
    Cancellation cancellation{cancelled};
    std::atomic<int> first_failure{-1};
    WorkerGroup workers{cancellation, {}};
    workers.threads.reserve(worker_count);
    std::exception_ptr startup_failure;
    int startup_device = -1;
    for (std::size_t index = 0; index < worker_count; ++index) {
        const auto pixel_begin = index * pixels_per_device + std::min(index, remainder);
        const auto pixel_end = pixel_begin + pixels_per_device + (index < remainder ? 1 : 0);
        const auto ordinal = static_cast<int>(index);
        try {
            workers.threads.emplace_back([&, index, ordinal, pixel_begin, pixel_end] {
                auto& outcome = outcomes[index];
                try {
                    outcome.succeeded = SolveRange(ordinal, pixel_begin, pixel_end, emitters, grid, inverse_r,
                                                   pending.phase_radians, cancellation, outcome.error);
                } catch (...) {
                    // Do not let an exception escape a worker, even when host
                    // allocation fails. Format the message after joining.
                    outcome.exception = std::current_exception();
                }
                if (!outcome.succeeded) {
                    int expected = -1;
                    first_failure.compare_exchange_strong(expected, ordinal, std::memory_order_relaxed);
                    cancellation.stopped.store(true, std::memory_order_release);
                }
            });
        } catch (...) {
            startup_failure = std::current_exception();
            startup_device = ordinal;
            cancellation.stopped.store(true, std::memory_order_release);
            break;
        }
    }
    workers.Join();
    if (!CheckCancelled(cancelled, error)) return false;
    if (startup_failure) {
        error = "CUDA device " + std::to_string(startup_device) +
                " worker startup failed: " + ExceptionMessage(startup_failure);
        return false;
    }
    const int failed_device = first_failure.load(std::memory_order_relaxed);
    if (failed_device >= 0) {
        const auto& outcome = outcomes[static_cast<std::size_t>(failed_device)];
        error = "CUDA device " + std::to_string(failed_device) + ": " +
                (outcome.exception ? ExceptionMessage(outcome.exception) : outcome.error);
        return false;
    }
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
        error = std::string("PointFocus host failure: ") + exception.what();
        return false;
    } catch (...) {
        error = "PointFocus failed with an unknown host exception.";
        return false;
    }
}
} // namespace cgh::solver::CudaPointFocus
