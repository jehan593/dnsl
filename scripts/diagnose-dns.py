#!/usr/bin/env python3
"""Read-only DNS path checks. Does not toggle protection or change networking."""
import concurrent.futures
import json
import socket
import ssl
import struct
import subprocess
import time

QUERY = b'\x47\x31\x01\x00\x00\x01\x00\x00\x00\x00\x00\x00' + b'\x07example\x03com\x00\x00\x01\x00\x01'

def exact(connection, size):
    result = b''
    while len(result) < size:
        block = connection.recv(size - len(result))
        if not block:
            raise RuntimeError('connection closed before DNS response')
        result += block
    return result

def summary(reply):
    if len(reply) < 12:
        raise RuntimeError('short DNS response')
    if reply[:2] != QUERY[:2]:
        raise RuntimeError('DNS response ID mismatch')
    return f'rcode={reply[3] & 15}, answers={int.from_bytes(reply[6:8], "big")}'

def udp(address, port):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as connection:
        connection.settimeout(4)
        connection.sendto(QUERY, (address, port))
        return summary(connection.recv(65535))

def tls(ip, port, hostname):
    with socket.create_connection((ip, port), timeout=4) as connection:
        with ssl.create_default_context().wrap_socket(connection, server_hostname=hostname) as secured:
            secured.sendall(struct.pack('!H', len(QUERY)) + QUERY)
            return summary(exact(secured, struct.unpack('!H', exact(secured, 2))[0]))

def check(label, function):
    start = time.monotonic()
    try:
        result = function()
    except Exception as error:
        result = f'{type(error).__name__}: {error}'
    return f'{label}: {result} ({time.monotonic() - start:.2f}s)'

checks = [('DNSL listener 127.0.0.1:15353', lambda: udp('127.0.0.1', 15353)),
          ('System DNS stub 127.0.0.53:53', lambda: udp('127.0.0.53', 53))]
try:
    with open('/etc/dnsl/settings.json') as source:
        settings = json.load(source)
    providers = [dict(id='cloudflare', ips=['1.1.1.1'], port=853, tlsHost='cloudflare-dns.com'),
                 dict(id='quad9', ips=['9.9.9.9'], port=853, tlsHost='dns.quad9.net')]
    providers += settings.get('customProviders', [])
    selected = next(p for p in providers if p['id'] == settings['selectedProviderId'])
    for ip in selected['ips']:
        checks.append((f'Selected provider TLS {ip}:{selected["port"]}',
                       lambda ip=ip: tls(ip, selected['port'], selected['tlsHost'])))
except Exception as error:
    print(f'Cannot read selected provider: {type(error).__name__}')
with concurrent.futures.ThreadPoolExecutor(max_workers=6) as executor:
    for result in executor.map(lambda item: check(*item), checks):
        print(result)
try:
    result = subprocess.run(['getent', 'ahostsv4', 'example.com'], timeout=5, capture_output=True, text=True)
    print(f'System NSS lookup: exit={result.returncode}, has_addresses={bool(result.stdout.strip())}')
except subprocess.TimeoutExpired:
    print('System NSS lookup: timeout after 5s')
