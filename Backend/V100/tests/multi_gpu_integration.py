#!/usr/bin/env python3
"""Compare real multi-GPU TCP solves with the same executable on one GPU.

Run: multi_gpu_integration.py /path/to/cgh_v100_server
Requires at least two visible CUDA GPUs; missing hardware is a failure, not a
skip. GPU selection belongs to Docker --gpus / CUDA_VISIBLE_DEVICES. Child
processes use driver-reported UUIDs so an inherited visibility mask is respected.
"""
import concurrent.futures
from contextlib import contextmanager
import copy
import ctypes
import os
import select
import struct
import sys
import time
import uuid

from integration import Server, frame, receive
from cuda_integration import (IDENTITIES, Sample, Scene, Target, encode,
                              mixed_scene, receive_result, solve)


def visible_devices():
    driver = ctypes.CDLL("libcuda.so.1")
    driver.cuInit.argtypes = [ctypes.c_uint]
    driver.cuDeviceGetCount.argtypes = [ctypes.POINTER(ctypes.c_int)]
    driver.cuDeviceGet.argtypes = [ctypes.POINTER(ctypes.c_int), ctypes.c_int]
    driver.cuDeviceGetName.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
    driver.cuDeviceGetUuid.argtypes = [ctypes.c_void_p, ctypes.c_int]

    def checked(operation, *arguments):
        status = operation(*arguments)
        assert status == 0, f"{operation.__name__} failed with CUDA status {status}"

    checked(driver.cuInit, 0)
    count = ctypes.c_int()
    checked(driver.cuDeviceGetCount, ctypes.byref(count))
    assert count.value >= 2, f"Multi-GPU coverage requires at least two visible GPUs, got {count.value}"
    devices = []
    for ordinal in range(count.value):
        device = ctypes.c_int()
        checked(driver.cuDeviceGet, ctypes.byref(device), ordinal)
        name = ctypes.create_string_buffer(256)
        checked(driver.cuDeviceGetName, name, len(name), device.value)
        device_uuid = (ctypes.c_ubyte * 16)()
        checked(driver.cuDeviceGetUuid, ctypes.byref(device_uuid), device.value)
        devices.append(("GPU-" + str(uuid.UUID(bytes=bytes(device_uuid))), name.value.decode("utf-8")))
    print("Visible CUDA devices: " + ", ".join(f"{name} ({identity})" for identity, name in devices), flush=True)
    return devices


@contextmanager
def server_process(executable, env=None):
    server = Server(executable, "--max-clients", "8", env=env)
    try:
        yield server
    finally:
        server.close()


def computed(server, scene, label):
    identity = next(IDENTITIES)
    with server.connect() as sock:
        sock.settimeout(15)
        sock.sendall(frame(1, identity, encode(scene)))
        return receive_result(sock, identity, scene, label)


def identical(actual, expected, label):
    assert len(actual) == len(expected), f"{label}: changed phase array length"
    assert struct.pack(f"!{len(actual)}d", *actual) == struct.pack(f"!{len(expected)}d", *expected), (
        f"{label}: single-GPU and multi-GPU phase arrays differ")
    print(f"PASS {label}: {len(actual)} phases are bit-identical", flush=True)


def sampled_scene(width, height, count):
    return Scene(width=width, height=height, direction=(0.8, 0.36, -0.48), incident_phase=-0.91,
                 targets=[Target(1, (0.5, -0.002, 0.003), samples=[
                     Sample((0.001 + index * 1.0e-8, (index % 17) * 1.0e-6, (index % 23) * 1.0e-6),
                            0.1 + (index % 11) / 10.0, 0.13 + (index % 7) / 10.0)
                     for index in range(count)
                 ])])


def parity(single, multiple):
    odd = mixed_scene()
    odd.width, odd.height = 7, 5  # The 17/18-pixel split cuts through a row.
    even = copy.deepcopy(odd)
    even.width, even.height = 8, 6
    posed = mixed_scene()
    posed.width, posed.height = 13, 9
    fixtures = [
        ("one pixel with more GPUs than pixels", Scene(width=1, height=1)),
        ("odd aperture and partition inside a row", odd),
        ("even aperture", even),
        ("rotated mesh with baked amplitudes and phases", posed),
    ]
    for label, scene in fixtures:
        expected = solve(single, scene, "single-GPU oracle: " + label)
        identical(computed(multiple, scene, label), expected, label)

    # 131,841 pixels put more than 65,536 pixels on each of two GPUs, so
    # partitions cut through a row and cross tile boundaries through 64K tiles.
    # 513 emitters also cross two 256-emitter batch boundaries. The single-GPU
    # path was separately checked against the unchanged CPU reference; avoid
    # millions of Python calls recomputing this large case's scalar oracle.
    batched = sampled_scene(513, 257, 513)
    expected = computed(single, batched, "single-GPU tile/batch baseline")
    identical(computed(multiple, batched, "multi-GPU tile/batch result"), expected,
              "multiple pixel tiles and emitter batches")
    return batched, expected



def inverse_r_parity(single, multiple):
    odd = mixed_scene()
    odd.algorithm, odd.width, odd.height = 3, 7, 5
    even = copy.deepcopy(odd)
    even.width, even.height = 8, 6
    fixtures = [
        ("inverse-r one pixel with more GPUs than pixels", Scene(width=1, height=1, algorithm=3)),
        ("inverse-r odd aperture and posed mesh", odd),
        ("inverse-r even aperture", even),
    ]
    for label, scene in fixtures:
        expected = solve(single, scene, "single-GPU inverse-r oracle: " + label)
        identical(computed(multiple, scene, label), expected, label)
    batched = sampled_scene(513, 257, 513)
    batched.algorithm = 3
    # Change weight exponents in later contributor batches, exercising persisted
    # max exponents and all three compensated states across launch boundaries.
    for index, sample in enumerate(batched.targets[0].samples):
        sample.amplitude *= 2.0 ** (3 * (index // 256))
    expected = computed(single, batched, "single-GPU inverse-r tile/batch baseline")
    identical(computed(multiple, batched, "multiple-GPU inverse-r tile/batch result"), expected,
              "inverse-r pixel tiles, contributor batches and dynamic exponent rescaling")
    return batched, expected


def concurrent_isolation(single, multiple, batched, baseline):
    variants = []
    for index in range(4):
        scene = copy.deepcopy(batched)
        scene.width, scene.height = 97 + 2 * index, 65 + 2 * index
        scene.incident_phase += 0.23 * (index + 1)
        scene.targets[0].position = (0.45 + 0.01 * index, -0.003 * index, 0.002 * index)
        variants.append(scene)
    expected = [computed(single, scene, f"concurrent baseline {index}")
                for index, scene in enumerate(variants)]
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(variants)) as pool:
        futures = [pool.submit(computed, multiple, scene, f"concurrent job {index}")
                   for index, scene in enumerate(variants)]
        for index, future in enumerate(futures):
            identical(future.result(timeout=20), expected[index], f"concurrent job {index} isolation")
    identical(computed(multiple, batched, "post-concurrency result"), baseline,
              "shared GPU state remains clean after concurrent jobs")


def cancellation_isolation(single, multiple, batched, baseline, algorithm=1):
    heavy = sampled_scene(1024, 512, 8192)
    # About 1.08 billion emitter/pixel interactions keep the survivor active
    # across the 20ms overlap window even with 64K pixel tiles. Assert below that
    # it has not sent a result before cancellation, rather than assuming overlap.
    survivor = sampled_scene(513, 257, 8193)
    survivor.incident_phase = 0.71
    heavy.algorithm = survivor.algorithm = algorithm
    expected = computed(single, survivor, "cancellation survivor baseline")
    heavy_payload, survivor_payload = encode(heavy), encode(survivor)
    cancelled_identity, survivor_identity = next(IDENTITIES), next(IDENTITIES)
    with multiple.connect() as cancelled, multiple.connect() as remaining:
        cancelled.settimeout(5)
        remaining.settimeout(15)
        cancelled.sendall(frame(1, cancelled_identity, heavy_payload))
        time.sleep(0.05)
        remaining.sendall(frame(1, survivor_identity, survivor_payload))
        time.sleep(0.02)
        ready, _, _ = select.select([remaining], [], [], 0)
        assert not ready, "Survivor completed before cancellation; fixture did not exercise overlapping jobs"
        started = time.monotonic()
        cancelled.sendall(frame(3, cancelled_identity))
        assert cancelled.recv(1) == b"", "Cancelled multi-GPU job emitted a partial result"
        elapsed = time.monotonic() - started
        assert elapsed < 2.0, f"Multi-GPU cancellation took {elapsed:.3f}s"
        phases = receive_result(remaining, survivor_identity, survivor, "uncancelled overlapping job")
        identical(phases, expected, "cancelling one job preserves the other job on both GPUs")
    print(f"PASS multi-GPU cancellation closes without partial result in {elapsed:.3f}s", flush=True)
    identical(computed(multiple, batched, "post-cancellation result"), baseline,
              "both GPUs recover after cancellation")


def no_device(executable):
    env = dict(os.environ, CUDA_VISIBLE_DEVICES="-1")
    with server_process(executable, env=env) as server:
        identity = next(IDENTITIES)
        with server.connect() as sock:
            sock.sendall(frame(1, identity, encode(Scene())))
            kind, reply_identity, payload = receive(sock)
            assert (kind, reply_identity) == (4, identity), "No visible CUDA GPU must return Error, never a dummy/CPU result"
            assert len(payload) >= 5
            length, = struct.unpack("!I", payload[:4])
            assert 0 < length <= 4096 and len(payload) == 4 + length
            diagnostic = payload[4:].decode("utf-8")
            assert "cuda" in diagnostic.lower(), f"Missing CUDA failure diagnostic: {diagnostic}"
            assert sock.recv(1) == b"", "CUDA failure was followed by a partial result"
        print(f"PASS zero visible GPUs reports Error without fallback: {diagnostic}", flush=True)


def main(executable):
    devices = visible_devices()
    single_environment = dict(os.environ, CUDA_VISIBLE_DEVICES=devices[0][0])
    with server_process(executable, env=single_environment) as single, server_process(executable) as multiple:
        batched, expected = parity(single, multiple)
        concurrent_isolation(single, multiple, batched, expected)
        cancellation_isolation(single, multiple, batched, expected)
        inverse_batched, inverse_expected = inverse_r_parity(single, multiple)
        concurrent_isolation(single, multiple, inverse_batched, inverse_expected)
        cancellation_isolation(single, multiple, inverse_batched, inverse_expected, algorithm=3)
    no_device(executable)
    print("Multi-GPU parity, partitioning, concurrency, cancellation and device-visibility checks passed", flush=True)


if __name__ == "__main__":
    assert len(sys.argv) == 2, "usage: multi_gpu_integration.py /path/to/cgh_v100_server"
    main(sys.argv[1])
