#!/usr/bin/env python3
"""Rejoue en RTP (format de ds5-usbip-proto --haptics-to) les canaux
haptiques 3-4 d'un enregistrement --dump (S16_LE, 4 canaux, 48 kHz), en
temps réel, avec un gain. Sert à comparer au ressenti « niveau » et
« contenu » des haptiques d'un jeu, sans le jeu.

Usage : tests/rtp_replay.py FICHIER IP [port] [début_s] [durée_s] [gain...]
Plusieurs gains : le passage est rejoué une fois par gain, avec une
seconde de silence entre deux.
"""
import array, random, socket, struct, sys, time

path, ip = sys.argv[1], sys.argv[2]
port = int(sys.argv[3]) if len(sys.argv) > 3 else 46100
start_s = float(sys.argv[4]) if len(sys.argv) > 4 else 0
dur_s = float(sys.argv[5]) if len(sys.argv) > 5 else 15
gains = [float(g) for g in sys.argv[6:]] or [1.0]

FRAME = 8  # 4 canaux × 2 octets
with open(path, "rb") as f:
    f.seek(int(start_s * 48000) * FRAME)
    raw = f.read(int(dur_s * 48000) * FRAME)
pcm = array.array("h")
pcm.frombytes(raw[: len(raw) // FRAME * FRAME])
left, right = pcm[2::4], pcm[3::4]

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
seq, ts, ssrc = random.getrandbits(16), random.getrandbits(32), random.getrandbits(32)


def clamp(v):
    return max(-32768, min(32767, int(v)))


def send(l, r):
    global seq, ts
    body = b"".join(struct.pack(">hh", a, b) for a, b in zip(l, r))
    s.sendto(struct.pack(">BBHII", 0x80, 127, seq & 0xFFFF, ts & 0xFFFFFFFF, ssrc) + body, (ip, port))
    seq += 1
    ts += len(l)


t0 = time.monotonic()
sent = 0
for gain in gains:
    peak = max(max(map(abs, left)), max(map(abs, right))) * gain
    print(f"gain ×{gain:g} (pic {min(peak, 32767):.0f})", flush=True)
    for i in range(0, len(left) - 47, 48):
        send([clamp(x * gain) for x in left[i : i + 48]], [clamp(x * gain) for x in right[i : i + 48]])
        sent += 48
        d = t0 + sent / 48000 - time.monotonic()
        if d > 0:
            time.sleep(d)
    for _ in range(1000):  # 1 s de silence
        send([0] * 48, [0] * 48)
        sent += 48
        d = t0 + sent / 48000 - time.monotonic()
        if d > 0:
            time.sleep(d)
print("fin", flush=True)
