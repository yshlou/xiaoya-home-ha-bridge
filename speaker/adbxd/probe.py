#!/usr/bin/env python3
"""Raw ADB-protocol probe: connect to <host>:<port> and dump the first bytes."""
import socket
import struct
import sys
import time

host = sys.argv[1] if len(sys.argv) > 1 else "192.168.0.185"
port = int(sys.argv[2]) if len(sys.argv) > 2 else 5555


def msg(cmd, a0, a1, payload=b""):
    return (cmd + struct.pack("<IIII", a0, a1, len(payload),
                              sum(payload) & 0xFFFFFFFF) + struct.pack("<I", 0) + payload)


def hexdump(b, n=64):
    return " ".join("%02x" % c for c in b[:n])


s = socket.create_connection((host, port), timeout=8)
print("[+] connected")

# 1) see whether the peer speaks first
s.settimeout(4)
try:
    first = s.recv(4096)
    print("[<] peer-first (%d bytes): %s" % (len(first), hexdump(first)))
    print("    ascii: %r" % first[:80])
except socket.timeout:
    print("[!] peer stayed silent for 4s -> we must speak first")

# 2) try to speak first: old-protocol CNXN, version 0x01000000, maxdata 4096
cnxn = msg(b"CNXN", 0x01000000, 4096, b"host::")
print("[>] sending CNXN:", hexdump(cnxn))
s.sendall(cnxn)
s.settimeout(6)
try:
    r = s.recv(4096)
    print("[<] reply (%d bytes): %s" % (len(r), hexdump(r)))
    print("    ascii: %r" % r[:120])
    if r[:4] == b"CNXN":
        print("[=] OK: standard CNXN handshake")
    elif r[:4] == b"AUTH":
        print("[=] peer demands AUTH")
    else:
        print("[=] unexpected command:", r[:4])
except socket.timeout:
    print("[!] no reply after CNXN")
s.close()
