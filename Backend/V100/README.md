# V100 backend transport scaffold

This directory builds a standalone Linux C++17 TCP server with **no Unreal Engine or CUDA dependency**. Phase 1 returns a deterministic dummy phase pattern. It does not run PointFocus or any optical algorithm. `PointFocus` in the request identifies the intended future solver; `DummySuccess` explicitly identifies the current response.

The portable [`include/cgh/wire.hpp`](include/cgh/wire.hpp) owns the versioned wire structs and bounded codecs. Unreal includes this header through its module include path, converts its owned solver snapshot, and receives a result into the normal job mailbox. The server never includes Unreal headers.

## Build and test

From the repository root:

```sh
cmake -S Backend/V100 -B Backend/V100/build -DCMAKE_BUILD_TYPE=Release
cmake --build Backend/V100/build --parallel
ctest --test-dir Backend/V100/build --output-on-failure
Backend/V100/build/cgh_v100_server --port 7000
```

CMake requires a C++17 compiler and pthreads. Tests also require Python 3 (standard library only). A server-only build can use `-DBUILD_TESTING=OFF`.

The tests cover byte-order fixtures, complete point/mesh snapshots, malformed/truncated payloads, invalid enums, revisions, nonfinite fields, size limits, partial TCP transfers, simultaneous clients, per-request correlation, receive deadlines, explicit Cancel, disconnect cancellation and cancellation while a large response is blocked by a slow receiver. The codec test compiles with exceptions and RTTI disabled, as required by the UE consumer.

## Docker

```sh
docker build -t cgh-v100-dummy Backend/V100
docker run --rm --name cgh-v100-dummy -p 127.0.0.1:7000:7000 cgh-v100-dummy
```

The image uses a multi-stage build and runs as UID 10001. It needs no GPU runtime. The server listens on all IPv4 interfaces inside the container. Publishing to host loopback keeps this development protocol local; CGHV 1.0 does not provide authentication, encryption, or cross-host discovery.

Select the actor's Docker backend and use IPv4 address `127.0.0.1`, port `7000`. Submit the existing Generate/Solve action. The usual `FCGHSolverJob -> PollSolver -> SLM` path applies the received phase pattern. CPU remains the default backend.

For horizontal column `x` and vertical row `y`, the dummy output is:

```text
phase[y * ResolutionX + x] = 2*pi * ((x + 3*y) mod 256) / 256
```

This is a visible nonuniform ramp for ordinary SLM sizes, with finite radians in `[0, 2*pi)`. A one-pixel SLM receives phase zero. Dimensions match the request; `ComputeSeconds` measures only dummy pattern generation, excluding artificial delay and network transfer.

## Lifecycle controls

```sh
cgh_v100_server --port 0 --delay-ms 500 --io-timeout-ms 30000 --max-clients 8
```

- `--port`: default 7000; zero chooses an available port. Readiness is a flushed `LISTENING <port>` stdout line.
- `--delay-ms`: default zero, maximum 300000; cancellable artificial delay for lifecycle tests.
- `--io-timeout-ms`: default 30000, range 1–300000; absolute budget for receiving a complete request and, separately, sending a response. Slow partial transfers cannot extend these budgets.
- `--max-clients`: default 8, range 1–128; bounds active connections. Excess connections are closed immediately.

Each connection carries one request and one response, then closes. Request IDs correlate both directions. A `Cancel` frame has no body and uses the same request ID; disconnect is authoritative cancellation. Cancellation is monitored while parsing, delaying, generating, encoding and sending, and worker ownership is independent of UE actors. No partial pattern is published after a cancelled or failed UE job. SIGINT/SIGTERM stop accepting work and cancel existing workers.

Protocol limits are 256 MiB per payload, 16384 per SLM axis, 16,777,216 total pixels, and 1,000,000 aggregate point emitters/mesh samples. These are Docker protocol limits; the existing CPU implementation retains its own limits. Large requests and responses require corresponding owned buffers on both sides; limit Docker memory or lower concurrency for the deployment workload.

See [PROTOCOL.md](PROTOCOL.md) for exact field order, stable enum values, resource semantics and future CUDA extension rules.
