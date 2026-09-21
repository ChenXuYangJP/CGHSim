FROM nvidia/cuda:12.9.2-devel-ubuntu22.04 AS build

RUN apt-get update && apt-get install -y \
    cmake \
    g++ \
    make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src

RUN cmake \
    -S . \
    -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CUDA_ARCHITECTURES=70 \
    -DBUILD_TESTING=OFF

RUN cmake --build build --parallel 2


FROM nvidia/cuda:12.9.2-runtime-ubuntu22.04

RUN useradd --system --uid 10001 --no-create-home cgh

COPY --from=build \
    /src/build/cgh_v100_server \
    /usr/local/bin/cgh_v100_server

USER cgh

EXPOSE 7000

ENTRYPOINT ["/usr/local/bin/cgh_v100_server"]
CMD ["--port", "7000"]
