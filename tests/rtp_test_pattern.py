#!/usr/bin/env python3
"""Envoie une vibration de test en RTP (même format que ds5-usbip-proto
--haptics-to) : sinus 80 Hz, 2 s à gauche, 2 s à droite, 2 s des deux.
Sert à vérifier la chaîne réseau → PipeWire → dongle sans jeu.

Usage : tests/rtp_test_pattern.py IP [port] [amplitude]
"""
import math, random, socket, struct, sys, time

ip = sys.argv[1]
port = int(sys.argv[2]) if len(sys.argv) > 2 else 46100
amp = int(sys.argv[3]) if len(sys.argv) > 3 else 20000
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
seq, ts, ssrc = random.getrandbits(16), random.getrandbits(32), random.getrandbits(32)
start = time.monotonic()
n = 0
for phase, (gl, gr) in enumerate(((1, 0), (0, 1), (1, 1))):
    print(("gauche", "droite", "les deux")[phase], flush=True)
    for _ in range(2000):  # 2 s, 1 ms par paquet
        body = bytearray()
        for i in range(48):
            v = int(amp * math.sin(2 * math.pi * 80 * (n + i) / 48000))
            body += struct.pack(">hh", v * gl, v * gr)
        hdr = struct.pack(">BBHII", 0x80, 127, seq & 0xFFFF, ts & 0xFFFFFFFF, ssrc)
        s.sendto(hdr + bytes(body), (ip, port))
        seq += 1; ts += 48; n += 48
        delay = start + n / 48000 - time.monotonic()
        if delay > 0:
            time.sleep(delay)
print("fin", flush=True)
