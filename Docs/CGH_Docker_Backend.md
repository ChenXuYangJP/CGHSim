# Docker/TCP backend — V100 PointFocus

`UCGHDockerSolverBackend` is the second implementation of `UCGHSolverBackend`. It serializes an owned solver snapshot, communicates asynchronously over TCP, and receives an `FCGHSolverResult`. `Backend/V100` contains an independent Linux C++ server, a CMake build, and a Dockerfile. The server and shared wire contract use no Unreal Engine headers or libraries.

**The server now runs the existing PointFocus algorithm on CUDA by default.** The portable solver lives in `src/solver/CudaPointFocus.cu`; `main.cpp` retains TCP framing and dispatch. `--solver dummy` selects the original transport-test ramp. `UCGHCPUSolverBackend` and the numerical implementation in `CGHPointFocus` are unchanged. The CPU implementation remains the numerical reference.

```mermaid
sequenceDiagram
    participant A as ACGHSolverActor (game thread)
    participant B as UCGHDockerSolverBackend worker
    participant S as Backend/V100 server
    participant J as FCGHSolverJob
    participant L as SLM
    A->>B: Submit(owned SI snapshot + endpoint settings)
    B->>S: TCP / CGHV v1 request with request ID
    S->>B: TCP / matching PointFocus result
    B->>J: Result, then release-store bFinished
    A->>J: PollSolver acquire-loads bFinished
    A->>A: Check input, endpoint, phase revision and convention
    A->>L: SetPhasePattern on game thread
```

## Build and run

Use the standalone [server instructions](../Backend/V100/README.md) for CMake, CTest, Docker image build, and port publishing. The default service port is **7000**. Native server builds require a **CUDA toolkit with `nvcc`** (the Docker build pins **12.9.2**), a compatible C++17 host compiler, CMake, and pthreads; CTest additionally uses Python 3. Unreal Engine is not required to build the server. The UE module itself retains its existing build dependencies.

`CGHV100Backend` project **1.1.0** enables CMake languages `CXX` and `CUDA`, targets architecture **70**, and includes `src/solver/CudaPointFocus.cu` with separable compilation. The solver ports the same FP64 point/mesh superposition, ordered compensated sums, single-emitter phase formula, zero-field threshold, pixel coordinates and illumination compensation as `CGHPointFocus.cpp`. It adds no new optical algorithm. The CPU backend and reference implementation remain unchanged; the explicit dummy mode is retained for transport tests.

The Docker build stage uses `nvidia/cuda:12.9.2-devel-ubuntu22.04`; its runtime stage uses `nvidia/cuda:12.9.2-runtime-ubuntu22.04`. Build and launch from the repository root:

```sh
docker build -t cgh-v100 Backend/V100
docker run --rm --gpus '"device=1"' --name cgh-v100 -p 127.0.0.1:7000:7000 cgh-v100
```

This GPU-enabled launch requires a compatible NVIDIA driver and NVIDIA Container Toolkit configured for Docker. The current host's physical GPU **1** is the **Tesla V100-SXM2-16GB**. Exposing only that GPU makes it logical **CUDA device 0** inside the container. Host GPU indices select the hardware made visible to Docker; CUDA device ordinals index the devices visible to the process. CUDA solver code must not hardcode host index 1. The CUDA solver selects this visible device 0. CUDA errors fail the request without a CPU or dummy fallback.

Build `CGHSimEditor` normally after adding the C++ files. The runtime module depends on `Sockets` and `Networking`; its private include path points to `Backend/V100/include`. This is an engine-independent protocol dependency, not a dependency on the server executable.

In Unreal:

1. Add/select **CGH Solver Actor** and assign its **Workbench**. Keep a valid SLM, PlaneWave light, and at least one valid Point or Mesh target, as for the CPU workflow.
2. Select **Parameters → Solver Backend → Docker (TCP/CUDA)**.
3. Set **Parameters → Docker → Address** to the server's numeric IPv4 address and **Port** to the published host port. Defaults are `127.0.0.1:7000`. DNS names are not supported in phase 1.
4. Set **Connect Timeout Seconds** and **Request Timeout Seconds** as needed (defaults 5 and 30 seconds). The request timeout includes serialization, connection, transfer, decoding, and conversion. No reconnect or automatic retry is performed within a job.
5. Click **Generate Phase Pattern**. Status moves through Queued/Running to Ready. The result status distinguishes a CUDA PointFocus calculation from an explicitly requested server dummy fixture. Select the SLM to view the received pattern.
6. Cancel during a request to retain the last accepted phase. Change back to CPU and generate to run the reference solver.

The Docker phase-1 limit is 16,777,216 pixels, at most 16,384 per axis, and 1,000,000 aggregate point/mesh samples. These transport limits do not change CPU limits. Each frame is limited to 256 MiB. Large grids require corresponding input, wire, result, and preview buffers; begin with the default 256×256 SLM.

## Ownership and lifecycle

Submission copies endpoint settings and the immutable scene/cloud snapshot into a job worker. Network I/O, serialization, and decoding happen off the game thread. The worker captures no actor or backend UObject and closes its socket when it completes. Only that worker writes `Result`; it release-stores `bFinished` afterwards. `PollSolver` acquire-loads completion before reading the result.

The actor retains its existing one-active/one-latest-pending policy. Replacing a request cancels the old worker before launching the replacement. Cancel, EndPlay, actor destruction, and failed or stale requests leave the previously accepted SLM phase intact. The game thread never joins or waits for the network worker. Endpoint changes and changes to transmitted scene fields invalidate Docker results; CPU consumed-input comparisons retain their prior semantics.

Each connection carries one request. A matching Cancel frame can be sent after the request has been completely transmitted; disconnect is authoritative cancellation at any point, including a partial request. Cancellation suppresses publication even when it races with a complete response. A cancelled or failed job contains no partial phase pattern. Socket operations use bounded waits and handle fragmented reads/writes; deadlines also cover a peer that stalls or disconnects.

The client checks version, type, request ID, lengths, dimensions, result status, propagation convention, finite compute time, and every phase sample before a result is eligible for SLM publication. The server validates the portable snapshot and reports protocol errors. No Unreal object layout, enum ordinal, pointer, or `FArchive` representation is sent over TCP.

## Wire protocol and CUDA extension

The wire protocol is now **CGHV 1.1**, independently of the CMake project version **1.1.0**. Version 1.1 adds the numerical `PointFocusSuccess` status; both UE and server must be rebuilt because old 1.0 peers reject it explicitly. See [the versioned wire specification](../Backend/V100/PROTOCOL.md) and the shared header in `Backend/V100/include/cgh/wire.hpp` for exact field order, units, type IDs, byte order, errors, and dummy pattern definition. Positions/pitches use meters, phase uses radians, and the propagation convention is explicit. Full binary64 values preserve optical precision; mesh clouds preserve identity, revision, target-local positions, normals, sample amplitude/phase, and UVs.

Phase storage remains `PhaseRad[row * ResolutionX + column]`, with columns along SLM-local +Y and rows along -Z. The result has no authority to set the SLM's local revision; `SetPhasePattern` owns that revision.

CUDA results use `PointFocusSuccess=2`, while the transport fixture uses `DummySuccess=1`. The numerical port preserves the CPU algorithm and emitter order, including Neumaier corrections across bounded GPU batches. CUDA and CPU math libraries may differ by rounding; parity is measured with circular phase error. Unknown versions, flags, and enums remain rejected. Future algorithms need explicit protocol support.

## Verification

Standalone protocol/server tests are registered with CTest. Unreal automation adds `CGH.DockerBackend` tests for transport failure, actor publication, CPU/Docker switching, cancellation, stale results, timeout, and recovery. Real-server tests require `-CGHDockerTestServer=/absolute/path/to/server`; without the option they log an explicit skip warning. The legacy dummy roundtrip test also accepts `-CGHDockerTestPort=<dummy-service-port>` to exercise a service explicitly started with `--solver dummy`. Numerical CUDA tests use `-CGHCudaTestPort=<cuda-service-port>` and compare returned phases directly with `CGHPointFocus::Solve`, then check actor-to-SLM publication. Ports refer to `127.0.0.1`.

### Recorded verification — 2026-09-21

**Historical transport milestone:** the results below were recorded with the earlier C++-only server and Ubuntu 24.04 image, before the CUDA 12.9.2 / Ubuntu 22.04 build scaffold. They establish the transport baseline and do not certify the updated CUDA image or GPU exposure.

- Linux Development Editor and Game builds succeeded.
- Standalone Release CMake build succeeded; CTest **2/2 passed**. The wire test also compiles without exceptions/RTTI. Independent Python clients exercise point/mesh requests, fragmented TCP, concurrency, invalid versions/fields/lengths/revisions, receive timeout, Cancel/disconnect and cancellation during a blocked 64 MiB response.
- All **63 CGH headless automation tests passed**, including all five new Docker tests, with no test skips or warnings in the automation report. Malformed-peer coverage includes an independently encoded fragmented valid response and eight rejection cases: request ID, version, convention, shape, payload size, NaN phase, infinite compute time, and truncated response.
- `RoundTripAndBackendSwitch` connected through an actual Docker container's published loopback port (`127.0.0.1:32769` in this run). It verified the complete actor → backend → TCP → container → TCP → job → PollSolver → SLM path, exact 5×3 dummy samples, publication only through polling, and CPU/Docker switching. Cancellation/stale-result/timeout tests launched separate native instances with controlled delay.
- `CGHCPUSolverBackend.h/.cpp` and `CGHPointFocus.h/.cpp` have no diff. SHA-256 comparison preserved all 23 pre-existing files under Content, including the user's already modified workbench map.
- The temporary verification container was stopped after testing. No GPU/CUDA computation, optical parity, graphical preview rendering, cooking or packaged deployment is claimed by this headless transport milestone.

### CUDA/V100 PointFocus verification — 2026-09-21

- The CUDA 12.9.2 / Ubuntu 22.04 Docker image built successfully for `sm_70`, including separable device linking. The runtime image ran with the exact `--gpus '"device=1"'` selection above. A CUDA API probe confirmed one visible device: logical device 0, Tesla V100-SXM2-16GB, compute capability 7.0.
- Linux Development Editor and Game builds succeeded. `CGHCPUSolverBackend.h/.cpp` and `CGHPointFocus.h/.cpp` have no diff.
- All **3/3 CTest suites passed** in the CUDA development container on GPU 1: wire codec, TCP integration, and CUDA PointFocus. Independent Python numerical fixtures, invalid optical inputs, in-flight cancellation, disconnect, and recovery all passed. Testing exposed and fixed a completed-client admission race; the single-client fixture now reconnects successfully after cancellation and EOF.
- All **66 CGH headless UE automation tests passed**, with zero failures, zero not-run tests, and no skip entries. The report records 59 clean successes and 7 successes with directory-watcher warnings from existing asset-save tests deleting their temporary directories. All three CUDA tests passed without warnings.
- Direct comparisons against the unchanged `CGHPointFocus::Solve` covered single and multiple points, even/odd/singleton grids, negative depth, oblique illumination, transformed mesh samples, large-amplitude normalization, ignored zero-amplitude points, destructive interference, and a 129×65 grid with 513 emitters crossing both GPU batch boundaries. The largest circular phase error in these fixtures was **1.4210854715202004e-14 radians**, below the test tolerance of `1e-7`. This measurement applies to the tested fixtures, not every possible scene.
- `CGH.CudaBackend.ActorToSLM` verified actor → Docker backend → TCP → actual V100 container → TCP → job → `PollSolver` → SLM. The final accepted SLM pattern matched the CPU reference within `8.8817841970012523e-16` radians. Validation, cancellation and recovery also passed through the real service.
- The final `cgh-v100:latest` image is available locally. The temporary runtime container was stopped after verification. These headless checks do not establish graphical preview rendering, cooking, packaged deployment, or performance at production scene sizes.

Local evidence (temporary machine-local files): `/tmp/cgh-v100-cuda-build.log`, `/tmp/cgh-v100-cuda-editor-build.log`, `/tmp/cgh-v100-cuda-game-build.log`, `/tmp/cgh-v100-cuda-ctest.log`, `/tmp/cgh-v100-cuda-tests/Testing/Temporary/LastTest.log`, `/tmp/cgh-v100-cuda-automation-final.log`, and `/tmp/cgh-v100-cuda-automation-final/index.json`.

### Reproduce transport checks

To reproduce from the repository root after building the Editor and standalone server with the current CUDA prerequisites:

```sh
CGHSIM_UE_ROOT="/path/to/UnrealEngine/UE5"
CGHSIM_PROJECT_ROOT="$PWD"

# With the CUDA Docker server running, run all CGH tests including CUDA parity:
"$CGHSIM_UE_ROOT/Engine/Binaries/Linux/UnrealEditor-Cmd" \
  "$CGHSIM_PROJECT_ROOT/CGHSim.uproject" \
  '-ExecCmds=Automation RunTests CGH; Quit' \
  "-CGHDockerTestServer=$CGHSIM_PROJECT_ROOT/Backend/V100/build/cgh_v100_server" \
  -CGHCudaTestPort=7000 \
  -unattended -nullrhi -nosplash

# Start the default CUDA Docker server, then run CUDA parity and publication tests:
"$CGHSIM_UE_ROOT/Engine/Binaries/Linux/UnrealEditor-Cmd" \
  "$CGHSIM_PROJECT_ROOT/CGHSim.uproject" \
  '-ExecCmds=Automation RunTests CGH.CudaBackend; Quit' \
  -CGHCudaTestPort=7000 -unattended -nullrhi -nosplash
```

The historical transport milestone's final full-suite run supplied both `-CGHDockerTestServer` and `-CGHDockerTestPort`, so roundtrip used Docker while controlled lifecycle tests used the native executable. Historical local evidence for that transport verification: `/tmp/cgh-v100-editor-build.log`, `/tmp/cgh-v100-game-build.log`, `/tmp/cgh-v100-docker-build.log`, `/tmp/cgh-v100-build/Testing/Temporary/LastTest.log`, `/tmp/cgh-v100-automation-final.log`, and `/tmp/cgh-v100-automation-final/index.json` (temporary machine-local files).
