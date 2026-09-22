#!/usr/bin/env python3
"""CGHV 1.2 CUDA observer reconstruction versus an independent complex oracle.

Requires real CUDA hardware; no missing-device skip or CPU/dummy fallback.
Pass an executable to test owned-process cancellation and no-device failures, or
--port for numerical checks against an existing loopback CUDA service.
"""
import argparse
import cmath
import copy
from dataclasses import dataclass, field
import itertools
import math
import os
import socket
import struct
import time

from integration import Server, frame, receive

TAU = 2.0 * math.pi
IDENTITIES = itertools.count(10001)


@dataclass
class Scene:
    width: int = 5
    height: int = 3
    pitch_x: float = 8.0e-6
    pitch_y: float = 11.0e-6
    observer_width: int = 7
    observer_height: int = 3
    observer_pitch_x: float = 19.0e-6
    observer_pitch_y: float = 23.0e-6
    observer_position: tuple = (0.31, 0.0013, -0.0021)
    observer_rotation: tuple = (0.0, 0.0, 0.0, 1.0)
    wavelength: float = 633.123456789e-9
    amplitude: float = 0.73
    initial_phase: float = -0.47
    direction: tuple = (1.0, 0.0, 0.0)
    source: int = 1
    source_position: tuple = (-0.15, 0.0011, -0.0017)
    modulation: int = 1
    phase: list = None

    def phases(self):
        return self.phase if self.phase is not None else [
            0.13 + 0.07 * (index % 7) - 0.04 * (index % 11) for index in range(self.width * self.height)]


def encode(scene):
    """Published 224-byte prefix followed by owned float64 phase samples."""
    payload = struct.pack("!IIII4dI", 2, 1, scene.width, scene.height,
                          scene.pitch_x, scene.pitch_y, scene.width * scene.pitch_x,
                          scene.height * scene.pitch_y, scene.modulation)
    payload += struct.pack("!6dI4d", scene.wavelength, scene.amplitude, scene.initial_phase,
                           *scene.direction, scene.source, *scene.source_position, 0.23)
    payload += struct.pack("!II9d", scene.observer_width, scene.observer_height,
                           scene.observer_pitch_x, scene.observer_pitch_y,
                           *scene.observer_position, *scene.observer_rotation)
    values = scene.phases()
    payload += struct.pack("!Q", len(values))
    assert len(payload) == 224
    return payload + struct.pack(f"!{len(values)}d", *values)


def rotated(q, v):
    # Independent quaternion-derived rotation matrix, rather than device cross products.
    x, y, z, w = q
    return ((1 - 2 * (y*y + z*z)) * v[0] + 2 * (x*y - z*w) * v[1] + 2 * (x*z + y*w) * v[2],
            2 * (x*y + z*w) * v[0] + (1 - 2 * (x*x + z*z)) * v[1] + 2 * (y*z - x*w) * v[2],
            2 * (x*z - y*w) * v[0] + 2 * (y*z + x*w) * v[1] + (1 - 2 * (x*x + y*y)) * v[2])


def oracle(scene, indices=None):
    """Evaluate RS-I with complex multiplication and independent math.fsum.

    Return the result and absolute contribution sum for cancellation-safe error
    bounds. No CUDA batching, compensated-state code or protocol codec is reused.
    """
    k = TAU / scene.wavelength
    area = scene.pitch_x * scene.pitch_y
    sources = []
    for index, phase in enumerate(scene.phases()):
        row, column = divmod(index, scene.width)
        p = (0.0, (column - (scene.width - 1) / 2) * scene.pitch_x,
             ((scene.height - 1) / 2 - row) * scene.pitch_y)
        if scene.source == 1:
            incident = scene.amplitude * cmath.exp(1j * (scene.initial_phase + k * sum(a*b for a, b in zip(scene.direction, p))))
        else:
            distance = math.dist(p, scene.source_position)
            incident = (scene.amplitude / distance) * cmath.exp(1j * (scene.initial_phase + k * distance))
        sources.append((p, incident * cmath.exp(1j * phase)))
    if indices is None:
        indices = range(scene.observer_width * scene.observer_height)
    results = []
    for index in indices:
        row, column = divmod(index, scene.observer_width)
        local = (0.0, (column - (scene.observer_width - 1) / 2) * scene.observer_pitch_x,
                 ((scene.observer_height - 1) / 2 - row) * scene.observer_pitch_y)
        q = tuple(a + b for a, b in zip(scene.observer_position, rotated(scene.observer_rotation, local)))
        contributions = []
        for p, incident in sources:
            distance = math.dist(q, p)
            kernel = cmath.exp(1j * k * distance) * (q[0] / (TAU * distance**2)) * (1 / distance - 1j * k)
            contributions.append(area * incident * kernel)
        value = complex(math.fsum(v.real for v in contributions), math.fsum(v.imag for v in contributions))
        results.append((value, math.fsum(abs(v) for v in contributions)))
    return results


def receive_result(sock, identity, scene, label):
    kind, received_identity, payload = receive(sock)
    assert (kind, received_identity) == (6, identity), f"{label}: expected reconstruction, got {kind}: {payload!r}"
    assert len(payload) >= 32
    status, convention, width, height, seconds, count = struct.unpack("!IIIIdQ", payload[:32])
    assert (status, convention, width, height, count) == (3, 1, scene.observer_width, scene.observer_height,
                                                       scene.observer_width * scene.observer_height)
    assert math.isfinite(seconds) and seconds >= 0
    assert len(payload) == 32 + count * 16
    samples = tuple(complex(real, imaginary) for real, imaginary in struct.iter_unpack("!dd", payload[32:]))
    assert all(math.isfinite(value.real) and math.isfinite(value.imag) for value in samples)
    assert sock.recv(1) == b"", "A completed connection must close without a second result"
    return samples


def computed(server, scene, label):
    identity = next(IDENTITIES)
    with server.connect() as sock:
        sock.settimeout(20)
        sock.sendall(frame(5, identity, encode(scene)))
        return receive_result(sock, identity, scene, label)


def check_oracle(samples, scene, label, indices=None):
    if indices is None:
        indices = list(range(len(samples)))
    expected = oracle(scene, indices)
    maximum = 0.0
    for index, (wanted, magnitude_sum) in zip(indices, expected):
        error = abs(samples[index] - wanted)
        # CPU/libm/CUDA distance and transcendental results can differ by a few
        # ulps at million-radian arguments. Bound against source magnitudes, not
        # the nearly-zero result of destructive interference.
        tolerance = 3.0e-8 * magnitude_sum + 5.0e-14
        assert error <= tolerance, f"{label}: pixel {index}, error {error:.12g} exceeds {tolerance:.12g}"
        maximum = max(maximum, error)
    print(f"PASS {label}: {len(indices)} oracle pixels, max complex error {maximum:.4g}", flush=True)


def solve(server, scene, label):
    samples = computed(server, scene, label)
    check_oracle(samples, scene, label)
    return samples


def reject_payload(server, payload, label, expected_text=None):
    identity = next(IDENTITIES)
    with server.connect() as sock:
        sock.settimeout(10)
        sock.sendall(frame(5, identity, payload))
        kind, received_identity, result = receive(sock)
        assert (kind, received_identity) == (4, identity), f"{label}: invalid request produced reconstruction"
        assert len(result) >= 5
        size, = struct.unpack("!I", result[:4])
        assert 0 < size <= 4096 and len(result) == 4 + size
        diagnostic = result[4:].decode("utf-8")
        assert diagnostic.strip()
        if expected_text:
            assert expected_text.lower() in diagnostic.lower(), diagnostic
        assert sock.recv(1) == b"", "Failure must not be followed by a partial result"
    print(f"PASS reject {label}: {diagnostic}", flush=True)


def reject(server, scene, label, expected_text=None):
    reject_payload(server, encode(scene), label, expected_text)


def tilted_scene():
    scene = Scene(direction=(0.8, 0.36, -0.48), initial_phase=0.79)
    axis = (1.0, 2.0, -3.0)
    scale = math.sin(0.63 / 2) / math.sqrt(sum(v*v for v in axis))
    scene.observer_rotation = tuple(v * scale for v in axis) + (math.cos(0.63 / 2),)
    return scene


def numerical_and_validation(server):
    solve(server, Scene(width=1, height=1, observer_width=1, observer_height=1), "single source/single observer sample")
    base = Scene()
    normal = solve(server, base, "asymmetric phase and observer grids")
    tilted = tilted_scene()
    solve(server, tilted, "oblique wave and tilted observer")
    point = copy.deepcopy(tilted); point.source = 2
    solve(server, point, "point-source illumination at one-meter amplitude reference")
    zero = copy.deepcopy(base); zero.amplitude = 0.0
    assert all(value == 0j for value in solve(server, zero, "zero source amplitude"))
    scaled = copy.deepcopy(base); scaled.amplitude *= 2
    doubled = solve(server, scaled, "absolute complex field amplitude retained")
    assert all(abs(a - 2*b) < 1.0e-12 for a, b in zip(doubled, normal))
    batched = Scene(width=257, height=1, observer_width=3, observer_height=2)
    solve(server, batched, "Kahan state spans 256-source boundary")
    tiled = Scene(width=3, height=2, observer_width=513, observer_height=257)
    tiled_samples = computed(server, tiled, "observer tile boundaries")
    indices = [0, 512, 513, 65535, 65536, 65919, 65920, 131071, 131072, len(tiled_samples)-1]
    check_oracle(tiled_samples, tiled, "global pixel coordinates across device/tile boundaries", indices)

    for field, value, label in [
        ("modulation", 2, "complex modulation"), ("direction", (2.0, 0.0, 0.0), "nonunit incident direction"),
        ("observer_rotation", (0.0, 0.0, 0.0, 2.0), "nonunit observer orientation"),
        ("observer_position", (0.0, 0.0, 0.0), "observer on SLM plane"),
        ("observer_position", (-0.1, 0.0, 0.0), "observer behind SLM"),
        ("amplitude", -1.0, "negative amplitude"), ("wavelength", float("nan"), "nonfinite wavelength"),
        ("observer_pitch_x", 0.0, "zero observer pitch"), ("phase", [0.0], "phase count mismatch"),
    ]:
        invalid = copy.deepcopy(base); setattr(invalid, field, value)
        reject(server, invalid, label)
    # Corner centers can still be finite even when the full grid extent overflows.
    overflowing_extent = Scene(width=1, height=1, observer_width=2, observer_height=1,
                               observer_pitch_x=1.0e308, wavelength=1.0e308)
    reject(server, overflowing_extent, "observer extent overflow matches CPU rejection")
    singular = Scene(width=1, height=1, source=2, source_position=(0.0, 0.0, 0.0))
    reject(server, singular, "source at SLM sample")
    invalid = copy.deepcopy(base); invalid.phase = invalid.phases(); invalid.phase[-1] = float("inf")
    reject(server, invalid, "nonfinite active phase")
    invalid = copy.deepcopy(base); invalid.observer_width = 4096; invalid.observer_height = 4096
    reject(server, invalid, "complex output exceeds 256MiB payload")
    reject_payload(server, encode(base) + b"extra", "trailing payload bytes")
    solve(server, base, "valid reconstruction recovers after invalid inputs")


def cancellation(executable):
    server = Server(executable, "--max-clients", "1")
    try:
        solve(server, Scene(), "cancellation fixture warmup")
        heavy = Scene(width=128, height=64, observer_width=1024, observer_height=512)
        payload = encode(heavy)
        identity = next(IDENTITIES)
        with server.connect() as sock:
            sock.settimeout(5)
            sock.sendall(frame(5, identity, payload))
            time.sleep(0.1)
            started = time.monotonic()
            sock.sendall(frame(3, identity))
            assert sock.recv(1) == b"", "Cancelled CUDA reconstruction emitted a partial result"
            elapsed = time.monotonic() - started
            assert elapsed < 2.0, f"Cancellation took {elapsed:.3f}s"
        time.sleep(0.2)
        solve(server, Scene(), "reconstruction recovers after cancellation")
        with server.connect() as sock:
            sock.sendall(frame(5, next(IDENTITIES), payload))
            time.sleep(0.05)
        time.sleep(0.3)
        solve(server, Scene(), "disconnect drains reconstruction worker buffers")
        print(f"PASS reconstruction cancellation in {elapsed:.3f}s", flush=True)
    finally:
        server.close()


def no_device(executable):
    environment = dict(os.environ, CUDA_VISIBLE_DEVICES="-1")
    server = Server(executable, env=environment)
    try:
        reject(server, Scene(), "no visible CUDA device has no CPU/dummy fallback", "CUDA")
    finally:
        server.close()


class ExternalServer:
    def __init__(self, port):
        self.port = port

    def connect(self):
        return socket.create_connection(("127.0.0.1", self.port), timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", nargs="?")
    parser.add_argument("--port", type=int)
    args = parser.parse_args()
    if bool(args.executable) == bool(args.port):
        parser.error("provide an executable or --port")
    if args.port:
        numerical_and_validation(ExternalServer(args.port))
    else:
        server = Server(args.executable)
        try:
            numerical_and_validation(server)
        finally:
            server.close()
        cancellation(args.executable)
        no_device(args.executable)
    print("CUDA reconstruction optical, validation and requested lifecycle checks passed", flush=True)


if __name__ == "__main__":
    main()
