# V100 PointFocus backend

A standalone Linux C++17/CUDA TCP server with no Unreal Engine headers or libraries. The default solver ports the existing [`CGHPointFocus.cpp`](../../Source/CGHSim/CGH/Solver/CGHPointFocus.cpp) algorithm to CUDA using FP64. The original CPU implementation remains the numerical reference. `--solver dummy` retains the deterministic transport fixture.

```text
src/
├── main.cpp                  # TCP, framing, decoding, cancellation, encoding
└── solver/
    ├── PointFocusSolver.hpp   # Portable solver boundary and dummy declaration
    ├── DummyPointFocus.cpp    # Original transport-test phase ramp
    ├── CudaPointFocus.hpp     # CUDA Solve declaration
    └── CudaPointFocus.cu      # Reference-equivalent PointFocus on the GPU

TCP -> DecodeRequest -> wire::Request -> CudaPointFocus::Solve
    -> wire::Result -> EncodeResult -> TCP
```

The portable [`include/cgh/wire.hpp`](include/cgh/wire.hpp) is shared with UE. The CMake project is `CGHV100Backend` **1.1.0**; the independently versioned wire protocol is **CGHV 1.1**. This protocol adds `PointFocusSuccess` alongside `DummySuccess`; version 1.0 peers reject the new protocol explicitly and must be rebuilt.

## Docker on the task's V100

From the repository root:

```sh
docker build -t cgh-v100 Backend/V100
docker run \
    --rm \
    --gpus '"device=1"' \
    --name cgh-v100 \
    -p 127.0.0.1:7000:7000 \
    cgh-v100
```

The build image is `nvidia/cuda:12.9.2-devel-ubuntu22.04`; the runtime image is `nvidia/cuda:12.9.2-runtime-ubuntu22.04`. CMake enables CXX/CUDA, architecture **70**, C++17/CUDA17, and separable compilation. CUDA fast math is disabled and multiply-add contraction is explicitly disabled to preserve the reference's arithmetic. The service runs as UID 10001.

The host needs a compatible NVIDIA driver and NVIDIA Container Toolkit configured for Docker. Physical GPU **1** on this host is the **Tesla V100-SXM2-16GB**. Docker exposes only that GPU, so the solver selects logical **CUDA device 0** inside the container. It must not use host index 1 as a CUDA device ordinal. The host does not need a native CUDA toolkit when building with Docker.

Select the UE actor's **Docker (TCP/CUDA)** backend and use `127.0.0.1:7000`. The normal `FCGHSolverJob -> PollSolver -> SLM` path applies the received result. CPU remains UE's default backend. CUDA failures are reported to UE; the server does not fall back to a CPU calculation or dummy pattern.

For the original transport fixture, append `--solver dummy` to the Docker command. Its phase remains `2*pi * ((column + 3*row) mod 256) / 256`. UE marks such a publication explicitly as dummy.

## Native build and tests

Native builds require CMake 3.18+, CUDA `nvcc`, a CUDA-compatible C++17 host compiler, and pthreads. The pinned Docker build uses CUDA 12.9.2. Tests additionally require Python 3 (standard library only). The UE module only consumes the portable wire header and gains no CUDA compiler dependency.

```sh
cmake -S Backend/V100 -B Backend/V100/build \
    -DCMAKE_BUILD_TYPE=Release -DCGH_ENABLE_CUDA_TESTS=ON
cmake --build Backend/V100/build --parallel
CUDA_VISIBLE_DEVICES=1 ctest --test-dir Backend/V100/build --output-on-failure
CUDA_VISIBLE_DEVICES=1 Backend/V100/build/cgh_v100_server --port 7000
```

If needed, pass `-DCMAKE_CUDA_COMPILER=/path/to/cuda-12.9/bin/nvcc`. `CGH_ENABLE_CUDA_TESTS` defaults to OFF so the codec and explicit dummy transport tests can run without a visible GPU; building the executable still requires the CUDA compiler. `-DBUILD_TESTING=OFF` builds only the server.

The tests cover the wire format, fragmented and malformed TCP, concurrent jobs, cancellation/disconnect/timeout, numerical point and mesh fixtures, plane-wave compensation, and invalid optical inputs. The UE CUDA tests compare received values directly against the unchanged `CGHPointFocus::Solve`; see [verification and UE test commands](../../Docs/CGH_Docker_Backend.md).

## Same PointFocus algorithm

- SLM pixel centers lie at local X=0; columns increase along +Y and rows along -Z. Positions and pitches are meters, phases radians.
- Positive point targets and transformed mesh samples contribute coherent complex fields with propagation `exp(-i*k*r)`. Mesh sample amplitude, phase, and scale are already baked into the resources and are not applied twice.
- Amplitudes are normalized by the global maximum. Real, imaginary and weight sums use Neumaier compensation, preserving source order. There is no `1/r` attenuation or sample-count normalization.
- A single positive emitter uses `target_phase - k*r - incident_phase`. Multiple emitters use `atan2(imaginary, real) - incident_phase`, except the reference's field-zero threshold selects exactly zero modulation.
- PlaneWave illumination and a phase-only SLM are required. The propagation convention, finite bounds, unit light direction, mesh quaternion, resource revisions, contributing points off the SLM plane, and positive contribution are validated before publication.
- GPU pixel/emitter batches retain the sum and compensation across launches. Cancellation is checked between bounded operations; the in-flight operation is drained before device buffers are released. A cancelled or failed job returns no partial result.

This is the same numerical algorithm on another device. GPU and CPU math libraries can differ by floating-point rounding; comparisons use circular phase error rather than bitwise equality. GS, FFT propagation, reconstruction imaging, and new optical models are outside this change.

## Lifecycle and limits

```sh
cgh_v100_server --port 0 --solver cuda --delay-ms 0 --io-timeout-ms 30000 --max-clients 8
```

- `--port`: default 7000; zero chooses an available port. Readiness prints `LISTENING <port>`.
- `--solver`: `cuda` (default) or `dummy`.
- `--delay-ms`: cancellable artificial delay for tests, 0–300000.
- `--io-timeout-ms`: receive and send budgets, separately; default 30000, range 1–300000. Partial transfers cannot extend them.
- `--max-clients`: default 8, range 1–128; excess connections close immediately.

Each connection carries one job. Cancel uses its request ID; disconnect is authoritative cancellation, including during partial reception. SIGINT/SIGTERM stop the listener and cancel workers. The client discards results for cancelled or superseded requests.

Limits remain 256 MiB per payload, 16384 per SLM axis, 16,777,216 total pixels, and 1,000,000 aggregate point/mesh samples. The unchanged CPU implementation retains its own limits. Memory and time scale with the requested grid and emitter count; start with the default 256×256 grid and coarse mesh sampling.

The server listens on all IPv4 interfaces inside its container; the shown command publishes only host loopback. CGHV does not supply authentication or encryption. See [PROTOCOL.md](PROTOCOL.md) for field order, units, enums, and version rules.
