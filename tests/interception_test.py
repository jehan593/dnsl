#!/usr/bin/env python3
"""No external traffic. --netns must run inside an isolated network namespace."""
import concurrent.futures
import os
import json
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time

netns = '--netns' in sys.argv
if netns:
    assert os.environ.get('DNSL_TEST_PARENT_NET') and \
        os.readlink('/proc/self/ns/net') != os.environ['DNSL_TEST_PARENT_NET'], \
        'Refusing to run kernel tests in the host network namespace; use make test-netns'
harness = subprocess.Popen([os.path.join(os.path.dirname(__file__), '../build/proxy_harness')],
                           stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
query = b'\x12\x34\x01\x00' + b'\x00' * 8

def command(cmd):
    harness.stdin.write(cmd + '\n')
    harness.stdin.flush()
    result = harness.stdout.readline().strip()
    assert result == 'OK', (cmd, result)

def udp(family, dest, port, sock=None, marker=0x41, payload=query):
    own = sock is None
    sock = sock or socket.socket(family, socket.SOCK_DGRAM)
    sock.settimeout(2)
    sock.sendto(payload, (dest, port))
    response, peer = sock.recvfrom(65535)
    assert response[:2] == payload[:2] and response[3] == marker, response
    assert peer[1] == port
    if own:
        sock.close()

def exact(sock, n):
    data = b''
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        assert chunk, 'unexpected TCP EOF'
        data += chunk
    return data

def tcp(family, dest, port, marker=0x41):
    with socket.socket(family, socket.SOCK_STREAM) as sock:
        sock.settimeout(2)
        sock.connect((dest, port))
        # Fragmented framing, repeated requests, and > old 4096 byte limit.
        for payload in (query, query + b'\x00' * 8000):
            packet = struct.pack('!H', len(payload)) + payload
            sock.sendall(packet[:1])
            sock.sendall(packet[1:])
            response = exact(sock, struct.unpack('!H', exact(sock, 2))[0])
            assert len(response) == len(payload) and response[3] == marker

try:
    if netns:
        subprocess.run(['ip', 'link', 'set', 'lo', 'up'], check=True)
        subprocess.run(['ip', 'link', 'add', 'vpn0', 'type', 'dummy'], check=True)
        subprocess.run(['ip', 'addr', 'add', '192.0.2.1/24', 'dev', 'vpn0'], check=True)
        subprocess.run(['ip', '-6', 'addr', 'add', '2001:db8::1/64', 'dev', 'vpn0', 'nodad'], check=True)
        subprocess.run(['ip', 'link', 'set', 'vpn0', 'up'], check=True)
        # Another owner's firewall table must survive every operation.
        subprocess.run(['nft', 'add', 'table', 'inet', 'vpn_owner'], check=True)
    if netns:
        initial_routes = subprocess.check_output(['ip', '-j', 'route', 'show', 'table', 'all'])
        initial_resolv_conf = open('/etc/resolv.conf', 'rb').read()
    command('start')
    for family, addr in ((socket.AF_INET, '127.0.0.1'), (socket.AF_INET6, '::1')):
        udp(family, addr, 15353)
        tcp(family, addr, 15353)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as size_test:
        size_test.settimeout(2)
        size_test.sendto(query + b'\0' * 1024, ('127.0.0.1', 15353))
        truncated = size_test.recv(65535)
        assert len(truncated) <= 512 and truncated[2] & 2, 'missing TCP fallback'
        # EDNS OPT with a 4096-byte advertised limit and a valid padding option.
        edns = query[:10] + b'\0\1' + b'\0' + struct.pack('!HHIH', 41, 4096, 0, 1028)
        edns += struct.pack('!HH', 12, 1024) + b'\0' * 1024
        size_test.sendto(edns, ('127.0.0.1', 15353))
        full = size_test.recv(65535)
        assert len(full) == len(edns) and not full[2] & 2
    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
        list(pool.map(lambda _: udp(socket.AF_INET, '127.0.0.1', 15353), range(64)))
    command('switch')
    tcp(socket.AF_INET, '127.0.0.1', 15353, marker=0x42)
    # A delayed old task must not answer through a descriptor reused by a new run.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as old:
        old.sendto(b'\xfe' + query[1:], ('127.0.0.1', 15353))
        time.sleep(0.03)
        command('stop')
        command('start')
        udp(socket.AF_INET, '127.0.0.1', 15353, marker=0x42)
        old.settimeout(0.4)
        try:
            old.recv(65535)
            raise AssertionError('old response escaped after stop')
        except (TimeoutError, ConnectionRefusedError):
            pass
    if netns:
        command('enable')
        for family, addr in ((socket.AF_INET, '192.0.2.53'), (socket.AF_INET6, '2001:db8::53')):
            udp(family, addr, 53, marker=0x42)
            tcp(family, addr, 53, marker=0x42)
        # A VPN interface created after Enable must be covered immediately.
        subprocess.run(['ip', 'link', 'add', 'vpn_later', 'type', 'dummy'], check=True)
        subprocess.run(['ip', 'addr', 'add', '198.51.100.1/24', 'dev', 'vpn_later'], check=True)
        subprocess.run(['ip', 'link', 'set', 'vpn_later', 'up'], check=True)
        udp(socket.AF_INET, '198.51.100.53', 53, marker=0x42)
        subprocess.run(['ip', 'link', 'del', 'vpn_later'], check=True)
        # Match resolved's UDP socket: select an outgoing interface, temporarily
        # bind for connect(), then unbind so replies may arrive over loopback.
        for family, addr, level, option in ((socket.AF_INET, '192.0.2.53', socket.IPPROTO_IP, 50),
                                           (socket.AF_INET6, '2001:db8::53', socket.IPPROTO_IPV6, 76)):
            with socket.socket(family, socket.SOCK_DGRAM) as bound:
                index = socket.if_nametoindex('vpn0')
                bound.setsockopt(level, option, socket.htonl(index))
                bound.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, b'vpn0\0')
                bound.connect((addr, 53))
                bound.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, b'')
                udp(family, addr, 53, sock=bound, marker=0x42)
        # Reuse a UDP 5-tuple through off -> on -> off with a normal DNS endpoint.
        for family, addr in ((socket.AF_INET, '127.0.0.2'), (socket.AF_INET6, '::1')):
            server = socket.socket(family, socket.SOCK_DGRAM)
            server.bind((addr, 53))
            def answer(s):
                while True:
                    try:
                        packet, peer = s.recvfrom(65535)
                        s.sendto(packet[:3] + b'\x55' + packet[4:], peer)
                    except OSError:
                        return
            threading.Thread(target=answer, args=(server,), daemon=True).start()
            with socket.socket(family, socket.SOCK_DGRAM) as client:
                udp(family, addr, 53, sock=client, marker=0x42)
                command('disable')
                udp(family, addr, 53, sock=client, marker=0x55)
                command('enable')
                udp(family, addr, 53, sock=client, marker=0x42)
                command('disable')
                udp(family, addr, 53, sock=client, marker=0x55)
            server.close()
            command('enable')
        command('disable')
        command('disable')  # Idempotent crash cleanup.
        subprocess.run(['nft', 'list', 'table', 'inet', 'vpn_owner'], check=True, stdout=subprocess.DEVNULL)
        tables = subprocess.check_output(['nft', 'list', 'tables'], text=True)
        assert 'dnsl_dns' not in tables, tables
    command('stop')
    command('quit')
    assert harness.wait(timeout=3) == 0
    if netns:
        environment = dict(os.environ, DBUS_SYSTEM_BUS_ADDRESS='unix:path=/nonexistent/dnsl-test-bus')
        lifecycle = subprocess.Popen([os.path.join(os.path.dirname(__file__), '../build/lifecycle_harness')],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=environment)
        control_path = lifecycle.stdout.readline().strip()
        clients = []
        def connect_tray():
            client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            client.settimeout(5)
            client.connect(control_path)
            clients.append(client)
            stream = client.makefile('r')
            status = json.loads(stream.readline())
            stream.close()
            assert status['enabled'], status
            return client
        def wait_state(expected):
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                lifecycle.stdin.write('status\n')
                lifecycle.stdin.flush()
                if lifecycle.stdout.readline().strip() == str(int(expected)):
                    return
                time.sleep(0.02)
            raise AssertionError('controller did not reach requested state')
        try:
            first = connect_tray()
            second = connect_tray()
            first.close()
            wait_state(True)
            second.close()
            wait_state(False)  # Last tray disconnect removes interception.
            assert 'dnsl_dns' not in subprocess.check_output(['nft', 'list', 'tables'], text=True)
            last = connect_tray()  # Remembered preference resumes on reopen.
            # Shutdown with a connected client must drain its worker before freeing IPC/controller.
            lifecycle.stdin.write('quit\n')
            lifecycle.stdin.flush()
            assert lifecycle.wait(timeout=5) == 0
            last.close()
            assert 'dnsl_dns' not in subprocess.check_output(['nft', 'list', 'tables'], text=True)
        finally:
            for client in clients:
                client.close()
            if lifecycle.poll() is None:
                lifecycle.kill()
                lifecycle.wait()
            shutil.rmtree(os.path.dirname(control_path), ignore_errors=True)
        assert subprocess.check_output(['ip', '-j', 'route', 'show', 'table', 'all']) == initial_routes
        assert open('/etc/resolv.conf', 'rb').read() == initial_resolv_conf
        # Simulate a daemon crash and run the exact ExecStopPost entry point.
        crashed = subprocess.Popen([os.path.join(os.path.dirname(__file__), '../build/proxy_harness')],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        for action in ('start', 'enable'):
            crashed.stdin.write(action + '\n')
            crashed.stdin.flush()
            assert crashed.stdout.readline().strip() == 'OK'
        udp(socket.AF_INET, '192.0.2.53', 53)
        crashed.kill()
        crashed.wait()
        subprocess.run([os.path.join(os.path.dirname(__file__), '../dnsl'), '--cleanup-network'],
                       env=environment, check=True)
        assert 'dnsl_dns' not in subprocess.check_output(['nft', 'list', 'tables'], text=True)
        subprocess.run(['nft', 'list', 'table', 'inet', 'vpn_owner'], check=True, stdout=subprocess.DEVNULL)
    print('PASS: proxy transports, provider switch, concurrency, shutdown' +
          (', kernel interception and DNS restoration' if netns else ''))
finally:
    if harness.poll() is None:
        harness.kill()
        harness.wait()
