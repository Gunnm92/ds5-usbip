#!/usr/bin/env python3
"""Écoute le flux RTP haptique de ds5-usbip-proto (--haptics-to) et vérifie
en-têtes et contenu : à lancer avant tests/usbip_client_test.py, qui envoie
1000 / -1000 sur les canaux 3-4.

Usage : tests/rtp_receiver_check.py [port] [durée_s]
"""
import socket, struct, sys

port = int(sys.argv[1]) if len(sys.argv) > 1 else 46000
dur = float(sys.argv[2]) if len(sys.argv) > 2 else 5
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.0.1", port))
s.settimeout(dur)
pkts = []
try:
    while True:
        pkts.append(s.recv(2048))
except socket.timeout:
    pass
ok = bool(pkts)
print(f"{len(pkts)} paquets RTP")
if pkts:
    v, pt, seq0, ts0, ssrc = pkts[0][0], pkts[0][1], *struct.unpack(">HII", pkts[0][2:12])
    print(f"version {v >> 6}, PT {pt & 0x7f}, {len(pkts[0]) - 12} octets de charge, ssrc {ssrc:08x}")
    ok &= v >> 6 == 2 and pt & 0x7F == 127 and len(pkts[0]) == 12 + 48 * 4
    for i, p in enumerate(pkts):
        seq, ts = struct.unpack(">HI", p[2:8])
        ok &= seq == (seq0 + i) & 0xFFFF and ts == (ts0 + 48 * i) & 0xFFFFFFFF
        samples = struct.unpack(f">{(len(p) - 12) // 2}h", p[12:])
        ok &= set(samples[0::2]) == {1000} and set(samples[1::2]) == {-1000}
print("OK" if ok else "ÉCHEC")
sys.exit(0 if ok else 1)
