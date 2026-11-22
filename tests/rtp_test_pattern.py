#!/usr/bin/env python3
"""Envoie une séquence de test en RTP (même format que ds5-usbip-proto /
Sunshine de sunshine-ds) pour vérifier la chaîne réseau → PipeWire → dongle
sans jeu.

2 canaux (défaut) : vibration 80 Hz, 2 s à gauche, 2 s à droite, 2 s des deux.
4 canaux (RTP_CHANNELS=4) : bip 440 Hz dans le haut-parleur gauche puis droit
(canaux 1-2), puis vibration 80 Hz gauche puis droite (canaux 3-4).

Usage : [RTP_CHANNELS=4] tests/rtp_test_pattern.py IP [port] [amplitude]
"""
import math, random, socket, struct, sys, time

ip = sys.argv[1]
port = int(sys.argv[2]) if len(sys.argv) > 2 else 46100
amp = int(sys.argv[3]) if len(sys.argv) > 3 else 20000
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
seq, ts, ssrc = random.getrandbits(16), random.getrandbits(32), random.getrandbits(32)
start = time.monotonic()
n = 0
import os
if os.environ.get("RTP_CHANNELS") == "4":
    # (nom, fréquence, gains des 4 canaux FL FR RL RR)
    phases = (("haut-parleur gauche", 440, (1, 0, 0, 0)), ("haut-parleur droit", 440, (0, 1, 0, 0)),
              ("haptique gauche", 80, (0, 0, 1, 0)), ("haptique droite", 80, (0, 0, 0, 1)))
else:
    phases = (("gauche", 80, (1, 0)), ("droite", 80, (0, 1)), ("les deux", 80, (1, 1)))
for name, freq, gains in phases:
    print(name, flush=True)
    for _ in range(2000):  # 2 s, 1 ms par paquet
        body = bytearray()
        for i in range(48):
            v = int(amp * math.sin(2 * math.pi * freq * (n + i) / 48000))
            body += struct.pack(">" + "h" * len(gains), *(v * g for g in gains))
        hdr = struct.pack(">BBHII", 0x80, 127, seq & 0xFFFF, ts & 0xFFFFFFFF, ssrc)
        s.sendto(hdr + bytes(body), (ip, port))
        seq += 1; ts += 48; n += 48
        delay = start + n / 48000 - time.monotonic()
        if delay > 0:
            time.sleep(delay)
print("fin", flush=True)
