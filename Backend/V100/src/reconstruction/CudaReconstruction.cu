#include "CudaReconstruction.hpp"

#include <cuda_runtime.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <thread>
#include <vector>

namespace cgh::reconstruction::CudaReconstruction {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t kPixelsPerTile = 65536;
constexpr std::size_t kContributorsPerBatch = 256;
constexpr std::size_t kUploadSourcesPerBatch = 8192;
constexpr unsigned kThreadsPerBlock = 128;

struct IncidentSample { double y, z, real, imaginary; };
struct Position { double x, y, z; };
struct Grid {
    std::uint32_t width, height;
    double pitch_x, pitch_y, k, area_over_two_pi;
    double x, y, z, qx, qy, qz, qw;
};
struct CompensatedSum {
    double sum = 0, correction = 0;
    __device__ void Add(double value) {
        // Preserve the CPU reference's Kahan evaluation and source order.
        const double adjusted = value - correction;
        const double next = sum + adjusted;
        correction = (next - sum) - adjusted;
        sum = next;
    }
};
struct PixelSums { CompensatedSum real, imaginary; };

bool CheckCancelled(const std::atomic<bool>& cancelled, std::string& error) {
    return !cancelled.load(std::memory_order_relaxed) || wire::Fail(error, "Reconstruction job cancelled.");
}

// Match the UE libc++ hypot evaluation in normal optical ranges, scaling only
// extreme magnitudes to avoid spurious overflow/underflow of squared terms.
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

__host__ __device__ Position ObserverPosition(std::size_t index, const Grid& grid) {
    const double y = (static_cast<double>(index % grid.width) - (grid.width - 1) / 2.0) * grid.pitch_x;
    const double z = ((grid.height - 1) / 2.0 - static_cast<double>(index / grid.width)) * grid.pitch_y;
    // FQuat::RotateVector: T = 2 * (Q cross V), V + W*T + (Q cross T).
    const double tx = 2.0 * (grid.qy * z - grid.qz * y);
    const double ty = 2.0 * (-grid.qx * z);
    const double tz = 2.0 * (grid.qx * y);
    return {grid.x + (grid.qw * tx + (grid.qy * tz - grid.qz * ty)),
            grid.y + ((y + grid.qw * ty) + (grid.qz * tx - grid.qx * tz)),
            grid.z + ((z + grid.qw * tz) + (grid.qx * ty - grid.qy * tx))};
}

bool GatherSources(const wire::ReconstructionRequest& request, std::vector<IncidentSample>& sources,
                   Grid& grid, std::string& error, const std::atomic<bool>& cancelled) {
    if (!CheckCancelled(cancelled, error) || !wire::ValidateReconstructionRequest(request, error, &cancelled)) return false;
    const auto& slm = request.slm;
    const auto& light = request.light;
    const auto& plane = request.observer;
    if (!std::isfinite(slm.resolution_x * slm.pixel_pitch_x_m) ||
        !std::isfinite(slm.resolution_y * slm.pixel_pitch_y_m) ||
        !std::isfinite(plane.resolution_x * plane.pixel_pitch_x_m) ||
        !std::isfinite(plane.resolution_y * plane.pixel_pitch_y_m))
        return wire::Fail(error, "SLM and observer pixel extents must be finite.");
    if (slm.modulation != wire::Modulation::PhaseOnly)
        return wire::Fail(error, "Reconstruction requires phase-only SLM modulation.");
    const double k = wire::kTwoPi / light.wavelength_m;
    const double area = slm.pixel_pitch_x_m * slm.pixel_pitch_y_m;
    if (!std::isfinite(k) || k <= 0 || !std::isfinite(area) || area <= 0)
        return wire::Fail(error, "Reconstruction wave number and pixel area must be finite and positive.");
    const auto& q = plane.rotation_slm;
    const double norm = ((q.x * q.x + q.y * q.y) + q.z * q.z) + q.w * q.w;
    if (!std::isfinite(norm) || std::abs(norm - 1.0) > 1.0e-6)
        return wire::Fail(error, "Observer rotation must be a unit quaternion.");
    if (light.source == wire::Source::PlaneWave) {
        const auto& d = light.direction_slm;
        const double length = (d.x * d.x + d.y * d.y) + d.z * d.z;
        if (!std::isfinite(length) || std::abs(length - 1.0) > 1.0e-6)
            return wire::Fail(error, "Plane-wave propagation direction must be unit length.");
    }
    grid = {plane.resolution_x, plane.resolution_y, plane.pixel_pitch_x_m, plane.pixel_pitch_y_m,
            k, area / wire::kTwoPi, plane.position_slm_m.x, plane.position_slm_m.y, plane.position_slm_m.z,
            q.x, q.y, q.z, q.w};
    // Affine pixel coordinates reach their extrema at the four corner centers.
    for (const std::size_t row : {std::size_t(0), std::size_t(plane.resolution_y - 1)}) {
        for (const std::size_t column : {std::size_t(0), std::size_t(plane.resolution_x - 1)}) {
            const Position point = ObserverPosition(row * plane.resolution_x + column, grid);
            if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z) || point.x <= 0)
                return wire::Fail(error, "Every observer pixel center must lie in front of the SLM (SLM-local X > 0).");
            for (const std::size_t source_row : {std::size_t(0), std::size_t(slm.resolution_y - 1)}) {
                for (const std::size_t source_column : {std::size_t(0), std::size_t(slm.resolution_x - 1)}) {
                    const double y = (static_cast<double>(source_column) - (slm.resolution_x - 1) / 2.0) * slm.pixel_pitch_x_m;
                    const double z = ((slm.resolution_y - 1) / 2.0 - static_cast<double>(source_row)) * slm.pixel_pitch_y_m;
                    const double distance = Distance(point.x, point.y - y, point.z - z);
                    if (!std::isfinite(distance) || !std::isfinite(k * distance))
                        return wire::Fail(error, "SLM-to-observer distance or phase exceeds the finite numerical range.");
                }
            }
        }
    }
    sources.resize(request.phase_radians.size());
    const double initial_phase = std::remainder(light.initial_phase_rad, wire::kTwoPi);
    for (std::size_t index = 0; index < sources.size(); ++index) {
        if ((index & 255) == 0 && !CheckCancelled(cancelled, error)) return false;
        auto& source = sources[index];
        source.y = (static_cast<double>(index % slm.resolution_x) - (slm.resolution_x - 1) / 2.0) * slm.pixel_pitch_x_m;
        source.z = ((slm.resolution_y - 1) / 2.0 - static_cast<double>(index / slm.resolution_x)) * slm.pixel_pitch_y_m;
        double amplitude = light.amplitude;
        double propagation;
        if (light.source == wire::Source::PointSource) {
            const double distance = Distance(light.position_slm_m.x,
                source.y - light.position_slm_m.y, source.z - light.position_slm_m.z);
            if (!std::isfinite(distance) || distance <= 0)
                return wire::Fail(error, "Point source must have finite nonzero distance from every SLM pixel center.");
            amplitude /= distance; // Source amplitude is specified at one meter.
            propagation = k * distance;
        } else {
            propagation = k * (light.direction_slm.y * source.y + light.direction_slm.z * source.z);
        }
        if (!std::isfinite(amplitude) || !std::isfinite(propagation))
            return wire::Fail(error, "Incident illumination exceeds the finite numerical range.");
        const double phase = initial_phase + std::remainder(propagation, wire::kTwoPi)
                           + std::remainder(request.phase_radians[index], wire::kTwoPi);
        source.real = amplitude * std::cos(phase);
        source.imaginary = amplitude * std::sin(phase);
    }
    return true;
}

bool CheckCuda(cudaError_t code, const char* operation, std::string& error) {
    if (code == cudaSuccess) return true;
    error = std::string("CUDA ") + operation + " failed: " + cudaGetErrorString(code);
    return false;
}

struct DeviceResources {
    cudaStream_t stream = nullptr;
    IncidentSample* sources = nullptr;
    PixelSums* sums = nullptr;
    wire::ComplexSample* samples = nullptr;
    int* numerical_error = nullptr;
    bool Release(std::string* error = nullptr) {
        bool succeeded = true;
        const auto release = [&succeeded, error](cudaError_t code, const char* operation) {
            if (code != cudaSuccess) {
                if (succeeded && error) CheckCuda(code, operation, *error);
                succeeded = false;
            }
        };
        // Host data and output remain alive until this stream has drained.
        if (stream) release(cudaStreamSynchronize(stream), "stream cleanup");
        if (numerical_error) { release(cudaFree(numerical_error), "error-buffer free"); numerical_error = nullptr; }
        if (samples) { release(cudaFree(samples), "sample-buffer free"); samples = nullptr; }
        if (sums) { release(cudaFree(sums), "sum-buffer free"); sums = nullptr; }
        if (sources) { release(cudaFree(sources), "source-buffer free"); sources = nullptr; }
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
        return !IsStopped() || wire::Fail(error, "Reconstruction job cancelled.");
    }
};

bool WaitForStream(cudaStream_t stream, const Cancellation& cancellation, std::string& error) {
    bool cancellation_seen = false;
    for (;;) {
        cancellation_seen |= cancellation.IsStopped();
        const auto status = cudaStreamQuery(stream);
        if (status == cudaSuccess) return !cancellation_seen || wire::Fail(error, "Reconstruction job cancelled.");
        if (status != cudaErrorNotReady) return CheckCuda(status, "stream completion", error);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

__global__ void AccumulateKernel(const IncidentSample* sources, std::size_t first, std::size_t source_count,
                                 Grid grid, std::size_t pixel_begin, std::size_t pixel_count,
                                 PixelSums* states, bool final_batch, wire::ComplexSample* samples, int* error) {
    const std::size_t local = std::size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (local >= pixel_count) return;
    const Position point = ObserverPosition(pixel_begin + local, grid);
    PixelSums state = states[local];
    for (std::size_t index = first; index < first + source_count; ++index) {
        const auto& source = sources[index];
        const double r = Distance(point.x, point.y - source.y, point.z - source.z);
        const double phase = grid.k * r;
        const double base = grid.area_over_two_pi * (point.x / r) / r;
        const double near = base / r;
        const double far = base * grid.k;
        if (!::isfinite(phase) || !::isfinite(near) || !::isfinite(far)) {
            atomicCAS(error, 0, 1);
            return;
        }
        const double cosine = ::cos(phase), sine = ::sin(phase);
        const double kernel_real = near * cosine + far * sine;
        const double kernel_imaginary = near * sine - far * cosine;
        state.real.Add(source.real * kernel_real - source.imaginary * kernel_imaginary);
        state.imaginary.Add(source.real * kernel_imaginary + source.imaginary * kernel_real);
    }
    states[local] = state;
    if (!final_batch) return;
    if (!::isfinite(state.real.sum) || !::isfinite(state.imaginary.sum)) {
        atomicCAS(error, 0, 2);
        return;
    }
    samples[local].real = state.real.sum;
    samples[local].imaginary = state.imaginary.sum;
}

bool SolveRange(int ordinal, std::size_t pixel_begin, std::size_t pixel_end,
                const std::vector<IncidentSample>& sources, const Grid& grid,
                std::vector<wire::ComplexSample>& samples, const Cancellation& cancellation,
                std::string& error) {
    if (!cancellation.Check(error) || !CheckCuda(cudaSetDevice(ordinal), "device selection", error)) return false;
    int numerical_error = 0; // Must outlive DeviceResources stream cleanup on every exit.
    DeviceResources device;
    if (!CheckCuda(cudaStreamCreateWithFlags(&device.stream, cudaStreamNonBlocking), "stream creation", error) ||
        !CheckCuda(cudaMalloc(reinterpret_cast<void**>(&device.sources), sources.size() * sizeof(IncidentSample)), "source allocation", error) ||
        !CheckCuda(cudaMalloc(reinterpret_cast<void**>(&device.sums), kPixelsPerTile * sizeof(PixelSums)), "sum allocation", error) ||
        !CheckCuda(cudaMalloc(reinterpret_cast<void**>(&device.samples), kPixelsPerTile * sizeof(wire::ComplexSample)), "sample allocation", error) ||
        !CheckCuda(cudaMalloc(reinterpret_cast<void**>(&device.numerical_error), sizeof(int)), "error allocation", error)) return false;
    for (std::size_t begin = 0; begin < sources.size(); begin += kUploadSourcesPerBatch) {
        if (!cancellation.Check(error)) return false;
        const auto count = std::min(kUploadSourcesPerBatch, sources.size() - begin);
        if (!CheckCuda(cudaMemcpyAsync(device.sources + begin, sources.data() + begin, count * sizeof(IncidentSample),
                                      cudaMemcpyHostToDevice, device.stream), "source upload", error) ||
            !WaitForStream(device.stream, cancellation, error)) return false;
    }
    for (std::size_t begin = pixel_begin; begin < pixel_end; begin += kPixelsPerTile) {
        if (!cancellation.Check(error)) return false;
        const auto count = std::min(kPixelsPerTile, pixel_end - begin);
        const auto blocks = static_cast<unsigned>((count + kThreadsPerBlock - 1) / kThreadsPerBlock);
        if (!CheckCuda(cudaMemsetAsync(device.numerical_error, 0, sizeof(int), device.stream), "clear numerical error", error) ||
            !CheckCuda(cudaMemsetAsync(device.sums, 0, count * sizeof(PixelSums), device.stream), "clear compensated sums", error)) return false;
        for (std::size_t first = 0; first < sources.size(); first += kContributorsPerBatch) {
            if (!cancellation.Check(error)) return false;
            const auto source_count = std::min(kContributorsPerBatch, sources.size() - first);
            AccumulateKernel<<<blocks, kThreadsPerBlock, 0, device.stream>>>(device.sources, first, source_count,
                grid, begin, count, device.sums, first + source_count == sources.size(), device.samples, device.numerical_error);
            numerical_error = 0;
            if (!CheckCuda(cudaGetLastError(), "diffraction kernel launch", error) ||
                !CheckCuda(cudaMemcpyAsync(&numerical_error, device.numerical_error, sizeof(int), cudaMemcpyDeviceToHost,
                                          device.stream), "numerical-error download", error) ||
                !WaitForStream(device.stream, cancellation, error)) return false;
            if (numerical_error == 1) return wire::Fail(error, "Diffraction kernel exceeds the finite numerical range.");
            if (numerical_error != 0) return wire::Fail(error, "Reconstructed complex field exceeds the finite numerical range.");
        }
        if (!CheckCuda(cudaMemcpyAsync(samples.data() + begin, device.samples, count * sizeof(wire::ComplexSample),
                                      cudaMemcpyDeviceToHost, device.stream), "field download", error) ||
            !WaitForStream(device.stream, cancellation, error)) return false;
    }
    return cancellation.Check(error) && device.Release(&error);
}

struct WorkerOutcome {
    bool succeeded = false;
    std::string error;
    std::exception_ptr exception;
};
struct WorkerGroup {
    Cancellation& cancellation;
    std::vector<std::thread> threads;
    void Join() { for (auto& thread : threads) if (thread.joinable()) thread.join(); }
    ~WorkerGroup() {
        cancellation.stopped.store(true, std::memory_order_release);
        Join();
    }
};
std::string ExceptionMessage(const std::exception_ptr& exception) {
    try { std::rethrow_exception(exception); }
    catch (const std::exception& failure) { return failure.what(); }
    catch (...) { return "Unknown host exception."; }
}

bool SolveInternal(const wire::ReconstructionRequest& request, wire::ReconstructionResult& result,
                   std::string& error, const std::atomic<bool>& cancelled) {
    const auto start = Clock::now();
    std::vector<IncidentSample> sources;
    Grid grid{};
    if (!GatherSources(request, sources, grid, error, cancelled) || !CheckCancelled(cancelled, error)) return false;
    int device_count = 0;
    if (!CheckCuda(cudaGetDeviceCount(&device_count), "device enumeration", error)) return false;
    if (device_count <= 0) return wire::Fail(error, "Reconstruction requires at least one visible CUDA device.");
    wire::ReconstructionResult pending;
    pending.status = wire::Status::ReconstructionSuccess;
    pending.convention = request.convention;
    pending.resolution_x = grid.width;
    pending.resolution_y = grid.height;
    const std::size_t total_pixels = std::size_t(grid.width) * grid.height;
    pending.samples.resize(total_pixels);
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
        const auto begin = index * pixels_per_device + std::min(index, remainder);
        const auto end = begin + pixels_per_device + (index < remainder ? 1 : 0);
        const auto ordinal = static_cast<int>(index);
        try {
            workers.threads.emplace_back([&, index, ordinal, begin, end] {
                auto& outcome = outcomes[index];
                try {
                    outcome.succeeded = SolveRange(ordinal, begin, end, sources, grid,
                                                   pending.samples, cancellation, outcome.error);
                } catch (...) { outcome.exception = std::current_exception(); }
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
        error = "CUDA device " + std::to_string(startup_device) + " worker startup failed: " + ExceptionMessage(startup_failure);
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
    if (!wire::ValidateReconstructionResult(pending, error, &cancelled)) return false;
    result = std::move(pending);
    return true;
}
} // namespace

bool Solve(const wire::ReconstructionRequest& request, wire::ReconstructionResult& result,
           std::string& error, const std::atomic<bool>& cancelled) {
    result = {};
    error.clear();
    try { return SolveInternal(request, result, error, cancelled); }
    catch (const std::exception& exception) {
        error = std::string("Reconstruction host failure: ") + exception.what();
        return false;
    } catch (...) {
        error = "Reconstruction failed with an unknown host exception.";
        return false;
    }
}
} // namespace cgh::reconstruction::CudaReconstruction
