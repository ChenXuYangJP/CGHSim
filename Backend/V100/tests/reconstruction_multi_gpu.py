#!/usr/bin/env python3
"""Compare CUDA reconstruction on one GPU and all visible GPUs, without fallback."""
import concurrent.futures
from contextlib import contextmanager
import copy
import os
import select
import struct
import sys
import time

from integration import Server, frame
from multi_gpu_integration import visible_devices
from reconstruction_integration import (IDENTITIES, Scene, check_oracle, computed, encode,
                                        receive_result, solve, tilted_scene)


@contextmanager
def server_process(executable, env=None):
    server = Server(executable, "--max-clients", "8", env=env)
    try:
        yield server
    finally:
        server.close()


def identical(actual, expected, label):
    assert len(actual) == len(expected), f"{label}: changed sample count"
    assert all(struct.pack("!dd", a.real, a.imag) == struct.pack("!dd", b.real, b.imag)
               for a, b in zip(actual, expected)), f"{label}: one/multiple GPU complex samples differ"
    print(f"PASS {label}: {len(actual)} complex samples are bit-identical", flush=True)


def parity(single, multiple):
    odd = tilted_scene()
    odd.observer_width, odd.observer_height = 7, 5
    even = copy.deepcopy(odd)
    even.observer_width, even.observer_height = 8, 6
    point = copy.deepcopy(odd)
    point.source = 2
    fixtures = [
        ("one pixel with more devices than pixels", Scene(width=1, height=1, observer_width=1, observer_height=1)),
        ("odd observer with split through a row", odd),
        ("even observer", even),
        ("point source with tilted observer", point),
    ]
    for label, scene in fixtures:
        expected = solve(single, scene, "one-GPU oracle: " + label)
        identical(computed(multiple, scene, label), expected, label)
    # 131841 observer samples split inside a row and cross 65536-sample tile
    # boundaries on both devices; 513 sources cross two contributor boundaries.
    batched = tilted_scene()
    batched.width, batched.height = 513, 1
    batched.observer_width, batched.observer_height = 513, 257
    expected = computed(single, batched, "one-GPU tile/batch baseline")
    indices = [0, 512, 513, 65535, 65536, 65919, 65920, 131071, 131072, len(expected)-1]
    check_oracle(expected, batched, "sampled oracle across tile and contributor boundaries", indices)
    identical(computed(multiple, batched, "multiple-GPU tile/batch result"), expected,
              "observer tiles and Kahan source batches")
    return batched, expected


def concurrent_isolation(single, multiple, batched, baseline):
    variants = []
    for index in range(4):
        scene = copy.deepcopy(batched)
        scene.observer_width, scene.observer_height = 97 + 2 * index, 65 + 2 * index
        scene.initial_phase += 0.23 * (index + 1)
        scene.observer_position = (0.35 + 0.01 * index, 0.001 * index, -0.002 * index)
        variants.append(scene)
    expected = [computed(single, scene, f"concurrent baseline {index}")
                for index, scene in enumerate(variants)]
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(variants)) as pool:
        futures = [pool.submit(computed, multiple, scene, f"concurrent job {index}")
                   for index, scene in enumerate(variants)]
        for index, future in enumerate(futures):
            identical(future.result(timeout=20), expected[index], f"concurrent reconstruction {index}")
    identical(computed(multiple, batched, "post-concurrency result"), baseline,
              "GPU resources remain isolated after concurrent reconstructions")


def cancellation_isolation(single, multiple, batched, baseline):
    heavy = Scene(width=128, height=64, observer_width=1024, observer_height=512)
    survivor = Scene(width=8193, height=1, observer_width=513, observer_height=257, initial_phase=0.71)
    expected = computed(single, survivor, "uncancelled overlap baseline")
    heavy_payload, survivor_payload = encode(heavy), encode(survivor)
    cancelled_identity, survivor_identity = next(IDENTITIES), next(IDENTITIES)
    with multiple.connect() as cancelled, multiple.connect() as remaining:
        cancelled.settimeout(5)
        remaining.settimeout(20)
        cancelled.sendall(frame(5, cancelled_identity, heavy_payload))
        time.sleep(0.05)
        remaining.sendall(frame(5, survivor_identity, survivor_payload))
        time.sleep(0.02)
        ready, _, _ = select.select([remaining], [], [], 0)
        assert not ready, "Overlap survivor completed too early to test in-flight cancellation isolation"
        started = time.monotonic()
        cancelled.sendall(frame(3, cancelled_identity))
        assert cancelled.recv(1) == b"", "Cancelled reconstruction emitted a partial field"
        elapsed = time.monotonic() - started
        assert elapsed < 2.0, f"Multiple-GPU cancellation took {elapsed:.3f}s"
        samples = receive_result(remaining, survivor_identity, survivor, "uncancelled overlapping reconstruction")
        identical(samples, expected, "cancelling one reconstruction preserves the other on all GPUs")
    print(f"PASS multiple-GPU reconstruction cancellation in {elapsed:.3f}s", flush=True)
    identical(computed(multiple, batched, "post-cancellation result"), baseline,
              "GPU buffers recover after reconstruction cancellation")


def main(executable):
    devices = visible_devices()
    environment = dict(os.environ, CUDA_VISIBLE_DEVICES=devices[0][0])
    with server_process(executable, env=environment) as single, server_process(executable) as multiple:
        batched, baseline = parity(single, multiple)
        concurrent_isolation(single, multiple, batched, baseline)
        cancellation_isolation(single, multiple, batched, baseline)
    print("Multiple-GPU reconstruction parity, tiling, isolation and cancellation checks passed", flush=True)


if __name__ == "__main__":
    assert len(sys.argv) == 2, "usage: reconstruction_multi_gpu.py /path/to/cgh_v100_server"
    main(sys.argv[1])
