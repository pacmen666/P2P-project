#!/usr/bin/env python3
"""Loopback tests, not evidence of a physical two-machine LAN test."""
import hashlib
import os
import pathlib
import socket
import struct
import subprocess
import threading

ROOT = pathlib.Path(__file__).resolve().parents[1]
EXE = pathlib.Path(os.environ.get('WEEK1_EXE', str(ROOT / 'build/week1/week1_peer.out')))
DIGEST = hashlib.sha256(bytes(16384)).hexdigest()

def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]

def positive(size):
    port = free_port()
    server = subprocess.Popen([str(EXE), 'serve', '127.0.0.1', str(port), str(size)],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        first = server.stdout.readline()
        assert first.startswith('LISTEN'), first
        client = subprocess.run([str(EXE), 'get', '127.0.0.1', str(port), str(size)],
                                capture_output=True, text=True, timeout=8)
        server_text = first + server.communicate(timeout=3)[0]
        assert client.returncode == server.returncode == 0, (client.stdout, client.stderr, server_text)
        assert 'BLOCK_OK bytes=16384' in client.stdout and DIGEST in client.stdout
        assert 'WAKE_OK' in client.stdout and 'WAKE_OK' in server_text
        assert 'cmd=0x05' in client.stdout
        if size <= 3:
            assert 'READ_HEADER waiting' in client.stdout
        print(f'PASS TCP loopback recv_size={size}, SHA256={DIGEST}, wake+heartbeat')
    finally:
        if server.poll() is None:
            server.kill()
            server.wait()

def frame(cmd, req, body):
    return struct.pack('!HBBHH', 0x4641, 1, cmd, req, len(body)) + body

def negative(kind):
    ready = socket.socket()
    ready.bind(('127.0.0.1', 0)); ready.listen(1); ready.settimeout(5)
    port = ready.getsockname()[1]
    errors = []
    def sender():
        try:
            with ready:
                conn, _ = ready.accept()
                with conn:
                    conn.settimeout(3)
                    data = b''
                    while len(data) < 25:
                        part = conn.recv(25-len(data))
                        if not part: raise AssertionError('request truncated')
                        data += part
                    assert data[:8] == bytes.fromhex('4641010a00010009')
                    content = bytearray(16384)
                    if kind == 'corrupt': content[123] = 1
                    body = struct.pack('!IBBHH', 1, 1, 0, 0, 16384) + content
                    response = frame(0x0b, 1, body)
                    if kind == 'truncated': response = response[:11]
                    if kind == 'wrong_session':
                        body = struct.pack('!IBBHH', 2, 1, 0, 0, 16384) + content
                        response = frame(0x0b, 1, body)
                    conn.sendall(response)
        except Exception as exc:
            errors.append(exc)
    thread = threading.Thread(target=sender); thread.start()
    try:
        result = subprocess.run([str(EXE), 'get', '127.0.0.1', str(port), '3'],
                                capture_output=True, text=True, timeout=8)
    finally:
        thread.join(timeout=6)
    assert not thread.is_alive() and not errors, errors
    assert result.returncode != 0 and 'BLOCK_OK' not in result.stdout
    if kind == 'corrupt': assert 'HASH_MISMATCH' in result.stderr
    if kind == 'truncated': assert 'TRUNCATED_FRAME' in result.stderr
    print(f'PASS rejects {kind} response')

for size in (1, 3, 4096): positive(size)
for case in ('corrupt', 'truncated', 'wrong_session'): negative(case)
print('ALL INTEGRATION TESTS PASS (single host, loopback only)')
