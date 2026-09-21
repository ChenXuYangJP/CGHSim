#!/usr/bin/env python3
"""Real CUDA TCP solver checked against an independent scalar Python oracle.

Run: cuda_integration.py /path/to/cgh_v100_server
Or:  cuda_integration.py --port 7000 (existing localhost CUDA service).
The executable path uses the server's default CUDA solver; GPU availability is
required. Select a physical device outside this script with CUDA_VISIBLE_DEVICES
or Docker --gpus. Missing CUDA never becomes a skipped/passing numerical test.
"""
import argparse
import copy
from dataclasses import dataclass, field
import itertools
import math
import socket
import struct
import sys
import time

from integration import Server, frame, receive

TAU = 2.0 * math.pi
PHASE_TOLERANCE = 1.0e-7  # CUDA/libm FP64 transcendental functions are not bit-identical.
IDENTITIES = itertools.count(1)


@dataclass
class Sample:
    position: tuple
    amplitude: float = 1.0
    phase: float = 0.0


@dataclass
class Target:
    identity: int
    position: tuple
    amplitude: float = 1.0
    phase: float = 0.0
    rotation: tuple = (0.0, 0.0, 0.0, 1.0)
    samples: list = None
    revision: int = 9
    cloud_revision: int = 9


@dataclass
class Scene:
    width: int = 5
    height: int = 3
    pitch_x: float = 80.0e-6
    pitch_y: float = 110.0e-6
    wavelength: float = 633.123456789e-9
    direction: tuple = (1.0, 0.0, 0.0)
    incident_phase: float = 0.2
    light_source: int = 1
    modulation: int = 1
    targets: list = field(default_factory=lambda: [Target(1, (0.5, 0.015, -0.024), 2.2, 0.73)])


def encode(scene):
    """Encode the published CGHV 1.1 layout without using the C++ codec."""
    payload = struct.pack("!IIIII4dI", 2, 1, 1, scene.width, scene.height,
                          scene.pitch_x, scene.pitch_y, scene.width * scene.pitch_x,
                          scene.height * scene.pitch_y, scene.modulation)
    payload += struct.pack("!6dI4d", scene.wavelength, 0.65, scene.incident_phase,
                           *scene.direction, scene.light_source, 0.0, 0.0, 0.0, 0.47)
    payload += struct.pack("!11dII", *([0.0] * 11), 0, 0)  # Optional camera absent.
    payload += struct.pack("!I", len(scene.targets))
    for target in scene.targets:
        payload += struct.pack("!QQ9dI", target.identity, target.revision,
                               *target.rotation, *target.position, target.amplitude,
                               target.phase, 2 if target.samples is not None else 1)
    meshes = [target for target in scene.targets if target.samples is not None]
    payload += struct.pack("!I", len(meshes))
    for target in meshes:
        payload += struct.pack("!QQI", target.identity, target.cloud_revision, len(target.samples))
        for sample in target.samples:
            payload += struct.pack("!10d", *sample.position, 0.0, 0.0, 1.0,
                                   sample.amplitude, sample.phase, 0.25, 0.75)
    return payload


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def rotate(quaternion, position):
    q = quaternion[:3]
    twice_cross = tuple(2.0 * value for value in cross(q, position))
    second_cross = cross(q, twice_cross)
    return tuple(position[i] + quaternion[3] * twice_cross[i] + second_cross[i] for i in range(3))


def wrap(phase):
    value = math.fmod(phase, TAU)
    if value < 0.0:
        value += TAU
    return 0.0 if value >= TAU or value == 0.0 else value


def oracle(scene):
    """Scalar equations and stable sums from the unchanged PointFocus contract.

    Mesh sample amplitudes/phases already include target material and scale.
    Source phasors are formed before propagation to retain opposing pi phases.
    math.fsum is an independent accurate sum, not a copy of CUDA accumulation.
    """
    emitters = []
    for target in scene.targets:
        if target.samples is None:
            if target.amplitude > 0.0:
                emitters.append((target.position, target.amplitude, target.phase))
        else:
            for sample in target.samples:
                if sample.amplitude > 0.0:
                    local = rotate(target.rotation, sample.position)
                    position = tuple(a + b for a, b in zip(target.position, local))
                    emitters.append((position, sample.amplitude, sample.phase))
    maximum = max(amplitude for _, amplitude, _ in emitters)
    weighted = [(position, amplitude / maximum, phase,
                 amplitude / maximum * math.cos(phase), amplitude / maximum * math.sin(phase))
                for position, amplitude, phase in emitters]
    threshold = 32.0 * sys.float_info.epsilon * math.fsum(item[1] for item in weighted)
    k = TAU / scene.wavelength
    phases = []
    for row in range(scene.height):
        z = ((scene.height - 1) / 2.0 - row) * scene.pitch_y
        for column in range(scene.width):
            y = (column - (scene.width - 1) / 2.0) * scene.pitch_x
            incident = scene.incident_phase + k * (scene.direction[1] * y + scene.direction[2] * z)
            if len(weighted) == 1:
                position, _, phase, _, _ = weighted[0]
                distance = math.hypot(position[0], position[1] - y, position[2] - z)
                result = phase - k * distance - incident
            else:
                real, imaginary = [], []
                for position, _, _, source_real, source_imaginary in weighted:
                    distance = math.hypot(position[0], position[1] - y, position[2] - z)
                    cosine, sine = math.cos(k * distance), math.sin(k * distance)
                    real.append(source_real * cosine + source_imaginary * sine)
                    imaginary.append(source_imaginary * cosine - source_real * sine)
                field_real, field_imaginary = math.fsum(real), math.fsum(imaginary)
                result = (0.0 if math.hypot(field_real, field_imaginary) <= threshold
                          else math.atan2(field_imaginary, field_real) - incident)
            phases.append(wrap(result))
    return phases


def receive_result(sock, identity, scene, label):
    """Validate one computed result and connection closure, without an oracle."""
    kind, reply_identity, payload = receive(sock)
    assert kind == 2, f"{label}: expected CUDA result, received kind {kind}: {payload!r}"
    assert reply_identity == identity, f"{label}: wrong request identity"
    assert len(payload) >= 32, f"{label}: truncated result metadata"
    status, convention, width, height, seconds, count = struct.unpack("!IIIIdQ", payload[:32])
    assert (status, convention, width, height, count) == (2, 1, scene.width, scene.height, scene.width * scene.height), (
        f"{label}: CUDA PointFocusSuccess metadata expected, got {status, convention, width, height, count}")
    assert math.isfinite(seconds) and seconds >= 0.0
    assert len(payload) == 32 + 8 * count
    phases = struct.unpack(f"!{count}d", payload[32:])
    assert all(math.isfinite(value) and 0.0 <= value < TAU for value in phases)
    assert sock.recv(1) == b"", "Server must close after one complete result"
    return phases


def solve(server, scene, label):
    identity = next(IDENTITIES)
    with server.connect() as sock:
        sock.sendall(frame(1, identity, encode(scene)))
        phases = receive_result(sock, identity, scene, label)
    count = len(phases)
    expected = oracle(scene)
    error = max(abs(math.remainder(actual - wanted, TAU)) for actual, wanted in zip(phases, expected))
    assert error <= PHASE_TOLERANCE, f"{label}: maximum wrapped phase error {error:.12g} rad"
    print(f"PASS {label}: {count} phases, max wrapped error {error:.3g} rad", flush=True)
    return phases


def reject(server, scene, label):
    identity = next(IDENTITIES)
    with server.connect() as sock:
        sock.sendall(frame(1, identity, encode(scene)))
        kind, reply_identity, payload = receive(sock)
        assert (kind, reply_identity) == (4, identity), f"{label}: invalid input returned a result/partial pattern"
        assert len(payload) >= 5
        length, = struct.unpack("!I", payload[:4])
        assert 0 < length <= 4096 and len(payload) == length + 4
        diagnostic = payload[4:].decode("utf-8")
        assert diagnostic.strip(), f"{label}: error must contain a diagnostic"
        assert sock.recv(1) == b"", f"{label}: error must not be followed by a partial result"
    print(f"PASS reject {label}: {diagnostic}", flush=True)


def mixed_scene():
    axis = (1.0, 2.0, -3.0)
    sine = math.sin(0.63 / 2.0) / math.sqrt(sum(value * value for value in axis))
    quaternion = tuple(value * sine for value in axis) + (math.cos(0.63 / 2.0),)
    return Scene(direction=(0.8, 0.36, -0.48), incident_phase=-0.91, targets=[
        Target(1, (0.5, 0.015, -0.024), 2.2, 0.73),
        Target(2, (-0.43, -0.013, 0.009), 0.65, 1.17),
        Target(3, (0.65, -0.011, 0.019), 0.0, 27.5, quaternion, [
            Sample((0.001, -0.002, 0.003), 0.31, -0.47),
            Sample((-0.002, 0.001, -0.0005), 0.73, 0.19),
            Sample((0.0003, -0.0012, 0.0009), 0.12, 1.41),
        ]),
    ])


def numerical_and_validation(server):
    base = Scene()
    normal = solve(server, base, "single point with asymmetric pixels")
    oblique = copy.deepcopy(base)
    oblique.direction, oblique.incident_phase = (0.8, 0.36, -0.48), -0.91
    solve(server, oblique, "oblique plane-wave compensation")
    backside = copy.deepcopy(base)
    backside.targets[0].position = (-0.35, 0.01, -0.02)
    solve(server, backside, "point behind SLM remains valid")
    zero_coplanar = copy.deepcopy(base)
    zero_coplanar.targets.append(Target(2, (0.0, 0.0, 0.0), 0.0, 0.31))
    assert solve(server, zero_coplanar, "zero-amplitude coplanar point ignored") == normal

    mixed = mixed_scene()
    mixed_phases = solve(server, mixed, "mixed point and posed mesh samples")
    changed_metadata = copy.deepcopy(mixed)
    changed_metadata.targets[-1].amplitude = 100.0
    changed_metadata.targets[-1].phase = -9.2
    assert solve(server, changed_metadata, "mesh sample optics already baked") == mixed_phases
    scaled = copy.deepcopy(mixed)
    for target in scaled.targets:
        if target.samples is None:
            target.amplitude *= 1.0e307
        else:
            for sample in target.samples:
                sample.amplitude *= 1.0e307
    scaled_phases = solve(server, scaled, "large finite amplitudes normalized before summation")
    assert max(abs(math.remainder(a - b, TAU)) for a, b in zip(scaled_phases, mixed_phases)) < PHASE_TOLERANCE

    cancelling = Scene(direction=(0.8, 0.36, -0.48), incident_phase=0.37, targets=[
        Target(1, (0.2, 0.005, -0.004), 1.0e308, 0.0),
        Target(2, (0.2, 0.005, -0.004), 1.0e308, math.pi),
    ])
    assert all(value == 0.0 for value in solve(server, cancelling, "destructive cancellation uses exact zero modulation"))

    invalid = copy.deepcopy(base); invalid.light_source = 2
    reject(server, invalid, "PointSource illumination")
    invalid = copy.deepcopy(base); invalid.modulation = 2
    reject(server, invalid, "complex SLM")
    invalid = copy.deepcopy(base); invalid.direction = (2.0, 0.0, 0.0)
    reject(server, invalid, "nonunit light direction")
    invalid = copy.deepcopy(mixed); invalid.targets[-1].rotation = (0.0, 0.0, 0.0, 2.0)
    reject(server, invalid, "nonunit mesh rotation")
    invalid = copy.deepcopy(base); invalid.targets[0].position = (0.0, 0.01, 0.02)
    reject(server, invalid, "positive-amplitude coplanar point")
    invalid = copy.deepcopy(base); invalid.targets[0].amplitude = 0.0
    reject(server, invalid, "all point amplitudes zero")
    invalid = copy.deepcopy(mixed); invalid.targets[-1].cloud_revision += 1
    reject(server, invalid, "stale mesh cloud revision")
    invalid = copy.deepcopy(mixed)
    for target in invalid.targets:
        target.amplitude = 0.0
        for sample in target.samples or []:
            sample.amplitude = 0.0
    reject(server, invalid, "all mesh sample amplitudes zero")
    solve(server, base, "valid CUDA request recovers after rejected inputs")


def cancellation(executable):
    # Only this test-owned process gets a heavy job; external services receive small checks.
    server = Server(executable, "--max-clients", "1")
    try:
        solve(server, Scene(), "CUDA cancellation fixture warmup")
        heavy = Scene(width=1024, height=512, targets=[Target(1, (0.5, 0.0, 0.0), samples=[
            Sample((0.001 + index * 1.0e-8, (index % 17) * 1.0e-6, (index % 23) * 1.0e-6), 1.0, 0.13)
            for index in range(8192)
        ])])
        payload = encode(heavy)
        identity = next(IDENTITIES)
        with server.connect() as sock:
            sock.sendall(frame(1, identity, payload))
            time.sleep(0.1)
            started = time.monotonic()
            sock.sendall(frame(3, identity))
            assert sock.recv(1) == b"", "Cancelled CUDA work must close without any partial result"
            elapsed = time.monotonic() - started
            assert elapsed < 2.0, f"CUDA cancellation took {elapsed:.3f}s"
        time.sleep(0.2)  # Allow listener to reap its only worker slot.
        solve(server, Scene(), "CUDA recovers after in-flight cancellation")
        with server.connect() as sock:
            sock.sendall(frame(1, next(IDENTITIES), payload))
            time.sleep(0.05)
        time.sleep(0.25)
        solve(server, Scene(), "CUDA disconnect releases worker and device buffers")
        print(f"PASS in-flight CUDA cancellation: {elapsed:.3f}s", flush=True)
    finally:
        server.close()


class ExternalServer:
    def __init__(self, port):
        self.port = port

    def connect(self):
        return socket.create_connection(("127.0.0.1", self.port), timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", nargs="?", help="cgh_v100_server executable (defaults to CUDA solver)")
    parser.add_argument("--port", type=int, help="check an existing localhost CUDA service; skip owned-process cancellation fixture")
    args = parser.parse_args()
    if bool(args.executable) == bool(args.port) or (args.port is not None and not 1 <= args.port <= 65535):
        parser.error("provide exactly one executable path or --port 1..65535")
    if args.port:
        numerical_and_validation(ExternalServer(args.port))
    else:
        server = Server(args.executable)
        try:
            numerical_and_validation(server)
        finally:
            server.close()
        cancellation(args.executable)
    print("CUDA PointFocus numerical, validation and requested lifecycle checks passed", flush=True)


if __name__ == "__main__":
    main()
