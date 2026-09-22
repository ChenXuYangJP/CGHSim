#!/usr/bin/env python3
"""Independent Python CGHV encoder exercises the real Linux TCP executable."""
import concurrent.futures
import math
import select
import socket
import struct
import subprocess
import sys
import time

HEADER = struct.Struct("!4sHHHHIQQ")


def frame(kind, request_id, payload=b"", major=1, minor=2, flags=0, reserved=0):
    return HEADER.pack(b"CGHV", major, minor, kind, flags, reserved, request_id, len(payload)) + payload


def request(width=7, height=5, mesh=True):
    data = struct.pack("!IIIII4dI", 2, 1, 1, width, height, 8e-6, 9e-6, width * 8e-6, height * 9e-6, 1)
    data += struct.pack("!6dI4d", 532e-9, 1, .2, 1, 0, 0, 1, 0, 0, 0, .3)
    data += struct.pack("!11dII", .05, 0, 0, 1, 0, 0, .05, 4, .5, .036, .024, 1920, 1080)
    data += struct.pack("!IQQ9dI", 1, 42, 9, 0, 0, 0, 1, .1, .02, .03, .8, -.2, 2 if mesh else 1)
    data += struct.pack("!I", 1 if mesh else 0)
    if mesh:
        data += struct.pack("!QQI10d", 42, 9, 1, .001, .002, .003, 0, 0, 1, .2, -.8, .25, .75)
    assert len(data) == (436 if mesh else 336)
    return data


def reconstruction_request():
    """Independent encoder: 224-byte CGHV 1.2 prefix, then raw SLM radians."""
    data = struct.pack("!IIII4dI", 2, 1, 2, 2, 8e-6, 9e-6, 16e-6, 18e-6, 1)
    data += struct.pack("!6dI4d", 532e-9, 1, .2, 1, 0, 0, 1, 0, 0, 0, .3)
    data += struct.pack("!II9dQ", 3, 2, 3e-6, 5e-6, .2, -.01, .03, 0, 0, .6, .8, 4)
    assert len(data) == 224
    data += struct.pack("!4d", 1.0, -0.0, -7.5, 1.25e100)
    return data


def exact(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise EOFError("Peer closed before complete frame")
        data.extend(chunk)
    return bytes(data)


def receive(sock):
    magic, major, minor, kind, flags, reserved, identity, length = HEADER.unpack(exact(sock, HEADER.size))
    assert (magic, major, minor, flags, reserved) == (b"CGHV", 1, 2, 0, 0)
    assert length <= 256 * 1024 * 1024
    return kind, identity, exact(sock, length)


class Server:
    def __init__(self, executable, *options, env=None):
        self.process = subprocess.Popen([executable, "--port", "0", *options], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
        ready, _, _ = select.select([self.process.stdout], [], [], 5)
        assert ready, "Server did not become ready"
        line = self.process.stdout.readline().strip()
        assert line.startswith("LISTENING "), line
        self.port = int(line.split()[1])

    def connect(self):
        sock = socket.create_connection(("127.0.0.1", self.port), timeout=3)
        sock.settimeout(3)
        return sock

    def close(self):
        self.process.terminate()
        self.process.communicate(timeout=5)
        assert self.process.returncode == 0


def check_result(kind, identity, payload, expected_id, width, height):
    assert kind == 2 and identity == expected_id
    status, convention, x, y, elapsed, count = struct.unpack("!IIIIdQ", payload[:32])
    assert (status, convention, x, y, count) == (1, 1, width, height, width * height)
    assert math.isfinite(elapsed) and elapsed >= 0
    assert len(payload) == 32 + 8 * count
    phases = struct.unpack("!" + str(count) + "d", payload[32:])
    for index, phase in enumerate(phases):
        assert 0 <= phase < 2 * math.pi
        row, column = divmod(index, width)
        assert abs(phase - 2 * math.pi * ((column + 3 * row) % 256) / 256) < 1e-14


def solve(server, identity, fragment=False):
    with server.connect() as sock:
        message = frame(1, identity, request())
        if fragment:
            for offset in range(0, len(message), 3):
                sock.sendall(message[offset:offset + 3])
        else:
            sock.sendall(message)
        check_result(*receive(sock), identity, 7, 5)
        assert sock.recv(1) == b""  # One job per connection.


def check_error(server, message):
    with server.connect() as sock:
        sock.sendall(message)
        kind, identity, payload = receive(sock)
        assert kind == 4 and identity == 99
        size, = struct.unpack("!I", payload[:4])
        assert 0 < size <= 4096 and len(payload) == size + 4
        return payload[4:].decode("utf-8")


def main(executable):
    server = Server(executable, "--solver", "dummy", "--io-timeout-ms", "300", "--max-clients", "8")
    try:
        solve(server, 1, fragment=True)
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(lambda identity: solve(server, identity), range(2, 18)))
        for changes in ({"major": 2}, {"minor": 0}, {"minor": 1}, {"minor": 3}, {"flags": 1}, {"reserved": 1}):
            check_error(server, frame(1, 99, request(), **changes))
        check_error(server, frame(8, 99))
        check_error(server, frame(2, 99))
        check_error(server, frame(6, 99))  # A reconstruction result cannot start a job.
        assert "dummy" in check_error(server, frame(5, 99, reconstruction_request())).lower()
        check_error(server, frame(5, 99))
        check_error(server, frame(5, 99, reconstruction_request() + b"x"))
        bad_reconstruction = bytearray(reconstruction_request()); struct.pack_into("!I", bad_reconstruction, 0, 1)
        assert "algorithm" in check_error(server, frame(5, 99, bad_reconstruction)).lower()
        bad_reconstruction = bytearray(reconstruction_request()); struct.pack_into("!Q", bad_reconstruction, 216, 2**64 - 1)
        assert "phase" in check_error(server, frame(5, 99, bad_reconstruction)).lower()
        bad_reconstruction = bytearray(reconstruction_request()); struct.pack_into("!d", bad_reconstruction, 224, float("nan"))
        assert "phase" in check_error(server, frame(5, 99, bad_reconstruction)).lower()
        bad_reconstruction = bytearray(reconstruction_request()); struct.pack_into("!d", bad_reconstruction, 184, float("inf"))
        assert "observer" in check_error(server, frame(5, 99, bad_reconstruction)).lower()
        bad_reconstruction = bytearray(reconstruction_request()); struct.pack_into("!II", bad_reconstruction, 136, 4096, 4096)
        check_error(server, frame(5, 99, bad_reconstruction))
        check_error(server, frame(1, 99, request() + b"x"))
        bad = bytearray(request()); struct.pack_into("!I", bad, 4, 77)
        check_error(server, frame(1, 99, bad))
        bad = bytearray(request()); struct.pack_into("!d", bad, 20, float("nan"))
        check_error(server, frame(1, 99, bad))
        bad = bytearray(request()); struct.pack_into("!I", bad, 236, 1000001)
        check_error(server, frame(1, 99, bad))
        bad = bytearray(request()); struct.pack_into("!Q", bad, 344, 10)
        check_error(server, frame(1, 99, bad))
        check_error(server, HEADER.pack(b"CGHV", 1, 2, 1, 0, 0, 99, 256 * 1024 * 1024 + 1))
        check_error(server, frame(1, 99, request(16385, 1)))
        with server.connect() as sock:
            sock.sendall(frame(1, 100, request())[:40])
            assert sock.recv(1) == b""  # Slow/truncated payload expires.
        with server.connect() as sock:
            sock.sendall(frame(5, 100, reconstruction_request())[:40])
            assert sock.recv(1) == b""  # Reconstruction uses the same body deadline.
        with server.connect() as sock:
            sock.sendall(b"CGH")  # Incomplete header then disconnect.
        solve(server, 101)
    finally:
        server.close()

    server = Server(executable, "--solver", "dummy", "--delay-ms", "500", "--max-clients", "1")
    try:
        with server.connect() as sock:
            sock.sendall(frame(1, 200, request()))
            time.sleep(.05)
            for byte in frame(3, 200):  # Fragmented Cancel.
                sock.sendall(bytes([byte]))
            start = time.monotonic()
            assert sock.recv(1) == b""
            assert time.monotonic() - start < .4
        time.sleep(.12)  # Listener reaps worker before accepting its sole slot.
        solve(server, 201)
        with server.connect() as sock:
            sock.sendall(frame(1, 202, request()))
        time.sleep(.15)
        solve(server, 203)  # Disconnect cancellation releases the only slot.
    finally:
        server.close()

    server = Server(executable, "--solver", "dummy", "--io-timeout-ms", "3000", "--max-clients", "1")
    try:
        with server.connect() as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
            sock.sendall(frame(1, 300, request(4096, 2048, mesh=False)))
            assert HEADER.unpack(exact(sock, 32))[3] == 2
            sock.sendall(frame(3, 300))  # Cancel while response send is blocked.
            time.sleep(.3)
            solve(server, 301)  # Slot is available despite unread large response.
    finally:
        server.close()
    print("TCP integration checks passed")


if __name__ == "__main__":
    main(sys.argv[1])
