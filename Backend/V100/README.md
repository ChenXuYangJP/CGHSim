# V100 solver and reconstruction backend

A standalone Linux C++17/CUDA TCP server with no Unreal Engine headers or libraries. The default solver ports the existing [`CGHPointFocus.cpp`](../../Source/CGHSim/CGH/Solver/CGHPointFocus.cpp) algorithm to CUDA using FP64. The service also reconstructs an observer-plane complex field using the FP64 Rayleigh–Sommerfeld calculation in [`CGHReconstruction.cpp`](../../Source/CGHSim/CGH/Reconstruction/CGHReconstruction.cpp). Both CPU implementations remain the numerical references. `--solver dummy` retains the deterministic transport fixture.

```text
src/
├── main.cpp                  # TCP, framing, decoding, cancellation, encoding
├── reconstruction/
│   ├── CudaReconstruction.hpp # Portable observer reconstruction boundary
│   └── CudaReconstruction.cu  # Rayleigh–Sommerfeld field across visible GPUs
└── solver/
    ├── PointFocusSolver.hpp   # Portable solver boundary and dummy declaration
    ├── DummyPointFocus.cpp    # Original transport-test phase ramp
    ├── CudaPointFocus.hpp     # CUDA Solve declaration
    └── CudaPointFocus.cu      # Reference-equivalent PointFocus across visible GPUs

TCP -> DecodeRequest -> wire::Request -> CudaPointFocus::Solve
    -> wire::Result -> EncodeResult -> TCP
```

The portable [`include/cgh/wire.hpp`](include/cgh/wire.hpp) is shared with UE. The CMake project is `CGHV100Backend` **1.4.0**; the independently versioned wire protocol is **CGHV 1.4**. Version 1.4 adds explicit thin-lens camera reconstruction requests/results. The inverse-distance PointFocus algorithm and status introduced in 1.3 remain supported. Separate reconstruction messages continue to carry complex samples independently of solver phases. Both Unreal and server must be rebuilt; older protocol versions are rejected explicitly.

## Docker on the task's two V100s

From the repository root:

```sh
docker build -t cgh-v100 Backend/V100
docker run \
    --rm \
    --gpus '"device=1,3"' \
    --name cgh-v100 \
    -p 127.0.0.1:7000:7000 \
    cgh-v100
```

The build image is `nvidia/cuda:12.9.2-devel-ubuntu22.04`; the runtime image is `nvidia/cuda:12.9.2-runtime-ubuntu22.04`. CMake enables CXX/CUDA, architecture **70**, C++17/CUDA17, and separable compilation. CUDA fast math is disabled and multiply-add contraction is explicitly disabled to preserve the reference's arithmetic. The service runs as UID 10001.

The host needs a compatible NVIDIA driver and NVIDIA Container Toolkit configured for Docker. Physical GPUs **1 and 3** on this host are the task's V100s. Docker exposes them as logical **CUDA devices 0 and 1** inside the container. The solver discovers and uses all visible CUDA devices; host GPU indices belong in the launch command, not in solver code. Exposing one GPU still works. The host does not need a native CUDA toolkit when building with Docker.

Select the UE actor's **Docker (TCP/CUDA)** backend and use `127.0.0.1:7000`. The normal `FCGHSolverJob -> PollSolver -> SLM` path applies the received result. CPU remains UE's default backend. In **Parameters → Algorithm**, select **PointFocus** (the existing constant-amplitude default) or **Point Focus (1/r Amplitude)**. Both run on the same service without restarting it. CUDA failures are reported to UE; the server does not fall back to a CPU calculation or dummy pattern.

For reconstruction, select **Reconstruction Backend = Docker (TCP/CUDA)** and **Mode = Observer Plane** on the reconstructor. Its **Docker** settings use the same endpoint, `127.0.0.1:7000`. Click **Reconstruct**, then select the observer for Phase, Amplitude, or Intensity. Both compute modes are available concurrently on the default CUDA service; no server restart or command-line mode switch is needed between them. Reconstruction accepts PlaneWave and PointSource illumination and returns the full complex field. For the camera, select **Mode = Camera (Thin Lens)**, assign the camera on the workbench, and configure its sensor and pupil sampling. Both reconstruction modes use the same endpoint. See the [camera model and sampling limits](../../Docs/CGH_Camera_Reconstruction.md).

For the original transport fixture, append `--solver dummy` to the Docker command. Its phase remains `2*pi * ((column + 3*row) mod 256) / 256`. UE marks such a publication explicitly as dummy. This fixture mode rejects reconstruction requests; it never fabricates a reconstructed field.

## Native build and tests

Native builds require CMake 3.18+, CUDA `nvcc`, a CUDA-compatible C++17 host compiler, and pthreads. The pinned Docker build uses CUDA 12.9.2. Tests additionally require Python 3 (standard library only). The UE module only consumes the portable wire header and gains no CUDA compiler dependency.

```sh
cmake -S Backend/V100 -B Backend/V100/build \
    -DCMAKE_BUILD_TYPE=Release -DCGH_ENABLE_MULTI_GPU_TESTS=ON
cmake --build Backend/V100/build --parallel
CUDA_VISIBLE_DEVICES=1,3 ctest --test-dir Backend/V100/build --output-on-failure
CUDA_VISIBLE_DEVICES=1,3 Backend/V100/build/cgh_v100_server --port 7000
```

If needed, pass `-DCMAKE_CUDA_COMPILER=/path/to/cuda-12.9/bin/nvcc`. `CGH_ENABLE_MULTI_GPU_TESTS` defaults to OFF; enabling it requires at least two visible GPUs and adds both the existing CUDA fixtures and the `cuda_multi_gpu` CTest suite. That suite compares single-GPU and dual-GPU results, checks cancellation while concurrent jobs run, and verifies the no-visible-GPU error. Reconstruction has separate numerical and multi-GPU suites enabled by the same options. For tests on one GPU, enable `CGH_ENABLE_CUDA_TESTS=ON` instead. Both options default to OFF so the codec and explicit dummy transport tests can run without a visible GPU; building the executable still requires the CUDA compiler. `-DBUILD_TESTING=OFF` builds only the server.

The tests cover the wire format, fragmented and malformed TCP, concurrent jobs, cancellation/disconnect/timeout, numerical point and mesh fixtures, plane-wave compensation, and invalid optical inputs. The UE CUDA tests compare received values directly against `CGHPointFocus::Solve` in both amplitude modes; see [verification and UE test commands](../../Docs/CGH_Docker_Backend.md).

## GPU partitioning

Each job divides the row-major pixel array into balanced, contiguous portions across the visible GPUs, using at most one device per output pixel. Each GPU receives the full emitter list and computes the complete ordered field sum for its pixels. Inputs are replicated, device buffers and streams belong to their device, and completed portions are copied into disjoint positions of one result array. No cross-GPU field reduction changes emitter order or compensation. The wire protocol and UE backend require no changes for multiple GPUs.

The two V100s on this host have six active NVLinks (`NV6` in `nvidia-smi topo -m`). This pixel partition needs no GPU-to-GPU transfer: each device reads its local emitter copy. NVLink peer copies could be evaluated later for distributing large emitter buffers; their benefit depends on measured transfer costs. See [NVIDIA peer-transfer documentation](https://docs.nvidia.com/cuda/cuda-programming-guide/03-advanced/multi-gpu-systems.html).

Full tiles contain 65,536 pixels and launch 512 blocks of 128 threads, giving the V100's 80 SMs multiple blocks to schedule. Emitter batches remain bounded at 256; smaller pixel partitions launch only the blocks needed. The measured tile-size comparison is recorded in the [backend verification guide](../../Docs/CGH_Docker_Backend.md).

Cancellation or a failure on any device aborts the whole job. All workers finish their bounded in-flight operations and release their own device resources before the job completes; a partial pattern is never published. GPU count does not change the optical calculation.

## PointFocus amplitude modes

- SLM pixel centers lie at local X=0; columns increase along +Y and rows along -Z. Positions and pitches are meters, phases radians.
- Positive point targets and transformed mesh samples contribute coherent complex fields with propagation `exp(-i*k*r)`. Mesh sample amplitude, phase, and scale are already baked into the resources and are not applied twice.
- **PointFocus** retains global-maximum amplitude normalization and the existing constant-amplitude complex sum.
- **Point Focus (1/r Amplitude)** sums `a_j/r_j * exp(i*(phi_j-k*r_j))`, where `r_j` is the distance from each point or transformed mesh sample to the current SLM pixel in meters. Binary mantissa/exponent scaling keeps relative weights finite without dividing amplitudes by a global maximum first. Real, imaginary and positive-weight sums use Neumaier compensation and are rescaled together when needed; the field-zero threshold uses that pixel's weight sum.
- Both modes preserve source order and omit sample-count normalization. A single positive emitter produces the same phase in both; several emitters at different distances can produce different phases. The weighting is `1/r` for field amplitude, not `1/r²` for intensity.
- A single positive emitter uses `target_phase - k*r - incident_phase`. Multiple emitters use `atan2(imaginary, real) - incident_phase`, except the reference's field-zero threshold selects exactly zero modulation.
- PlaneWave illumination and a phase-only SLM are required. The propagation convention, finite bounds, unit light direction, mesh quaternion, resource revisions, contributing points off the SLM plane, and positive contribution are validated before publication.
- GPU pixel/emitter batches retain the sum and compensation across launches. Cancellation is checked between bounded operations; the in-flight operation is drained before device buffers are released. A cancelled or failed job returns no partial result.

This is the same numerical algorithm distributed by pixel across devices. GPU and CPU math libraries can differ by floating-point rounding; comparisons use circular phase error rather than bitwise equality. GS and FFT propagation are outside these direct-summation implementations.

## Observer reconstruction

The reconstruction request carries SLM sampling, the active phase array, illumination, and the observer's sampling grid and rigid pose in SLM-local meters. Each observer output is the complete ordered sum over SLM pixels. The result contains one `(real, imaginary)` binary64 pair per observer pixel, without normalization. Preview and saved PNG mappings remain in Unreal.

The CUDA implementation follows the [CPU reconstruction model](../../Docs/CGH_Reconstruction.md#cpu-propagation-model), including point-source attenuation, tilted planes, and the `exp(+i*k*r)` convention. It partitions **observer output pixels** across visible devices and replicates incident SLM samples. Ordered compensated real/imaginary sums persist across contributor batches; GPUs do not reduce partial fields into one another. Cancellation drains in-flight work and publishes no partial field.

Reconstruction starts from the measured PointFocus launch configuration: **65,536 output pixels per tile, 128 threads per block, and 256 source pixels per accumulation batch**. A full tile launches 512 blocks, exceeding the V100's 80 SMs. Both PointFocus modes retain these measured launch settings. These are launch settings for direct propagation, not an optical truncation of the aperture; every SLM pixel contributes. This does not claim that the PointFocus measurements establish an optimal tile size for reconstruction.

## Camera reconstruction

The camera path reuses the observer Rayleigh–Sommerfeld kernel twice: SLM to the circular pupil, then pupil to the sensor after applying the thin-lens phase. Focal length, f-number, focus distance, complete optical quaternion, sensor resolution/pitch, and pupil resolution are explicit request data. The first GPU pass finishes before row-major pupil clipping/phase and the second pass; cancellation covers both. Both passes retain 65,536-pixel tiles, 128-thread blocks, and 256-contributor batches. Sensor results are complex binary64 pairs with their own message type and success status. Pupil sampling is limited to 2048 per axis and requires convergence checks; a coarse grid can severely alias.

## Lifecycle and limits

```sh
cgh_v100_server --port 0 --solver cuda --delay-ms 0 --io-timeout-ms 30000 --max-clients 8
```

- `--port`: default 7000; zero chooses an available port. Readiness prints `LISTENING <port>`.
- `--solver`: `cuda` (default, dispatches both PointFocus modes, observer reconstruction, and camera reconstruction) or `dummy` (PointFocus transport fixture only).
- `--delay-ms`: cancellable artificial delay for tests, 0–300000.
- `--io-timeout-ms`: receive and send budgets, separately; default 30000, range 1–300000. Partial transfers cannot extend them.
- `--max-clients`: default 8, range 1–128; excess connections close immediately.

Each connection carries one job. Cancel uses its request ID; disconnect is authoritative cancellation, including during partial reception. SIGINT/SIGTERM stop the listener and cancel workers. The client discards results for cancelled or superseded requests.

Limits remain 256 MiB per payload, 16384 per SLM axis, 16,777,216 total pixels, and 1,000,000 aggregate point/mesh samples. Reconstruction permits up to 16,777,216 SLM pixels and 16,777,214 observer pixels (the 32-byte result prefix plus 16 bytes per complex sample must fit the same payload cap). Each axis remains bounded by 16384. The CPU implementations retain their own limits. Memory and time scale with the requested grid and emitter count; start with the default 256×256 grid and coarse mesh sampling.

The server listens on all IPv4 interfaces inside its container; the shown command publishes only host loopback. CGHV does not supply authentication or encryption. See [PROTOCOL.md](PROTOCOL.md) for field order, units, enums, and version rules.
