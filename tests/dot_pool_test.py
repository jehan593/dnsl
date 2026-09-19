#!/usr/bin/env python3
"""Exercise real certificate verification, DoT framing and cancellation on loopback."""
import os
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import tempfile
import threading

with tempfile.TemporaryDirectory(prefix='dnsl-tls-test-') as directory:
    cert = str(Path(directory) / 'cert.pem')
    key = str(Path(directory) / 'key.pem')
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                    '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost',
                    '-keyout', key, '-out', cert], check=True, capture_output=True)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    blocked = threading.Event()
    errors = []
    def exact(conn, n):
        result = b''
        while len(result) < n:
            chunk = conn.recv(n - len(result))
            if not chunk:
                raise EOFError('peer closed early')
            result += chunk
        return result
    def serve(listener):
        try:
            conn, _ = listener.accept()
            with context.wrap_socket(conn, server_side=True) as tls:
                n = struct.unpack('!H', exact(tls, 2))[0]
                q = exact(tls, n)
                tls.sendall(struct.pack('!H', n) + q[:2] + b'\x81' + q[3:])
                n = struct.unpack('!H', exact(tls, 2))[0]
                assert exact(tls, n)[0] == 0xfe
                blocked.set()
                assert tls.recv(1) == b''  # cancel must interrupt the outstanding read
        except Exception as error:
            errors.append(error)
            blocked.set()
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        listener.listen()
        listener.settimeout(5)
        thread = threading.Thread(target=serve, args=(listener,), daemon=True)
        thread.start()
        binary = Path(__file__).resolve().parent.parent / 'build/dot_pool_test'
        process = subprocess.Popen([str(binary), str(listener.getsockname()[1])], stdin=subprocess.PIPE,
                                   env=dict(os.environ, SSL_CERT_FILE=cert))
        try:
            assert blocked.wait(5), 'TLS query did not reach the server'
            assert not errors, errors
            process.communicate(b'c', timeout=3)
            assert process.returncode == 0
            thread.join(timeout=3)
            assert not thread.is_alive() and not errors, errors
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    print('PASS: verified local TLS, DNS framing, immediate cancellation of in-flight query')
