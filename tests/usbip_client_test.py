#!/usr/bin/env python3
"""Client USB/IP minimal qui imite vhci-hcd, pour tester ds5-usbip-proto sans
l'hôte : devlist, import, descripteurs, rapports feature, HID IN, isochrone
OUT rythmé, isochrone IN, UNLINK.

Usage : tests/usbip_client_test.py [hôte] [port]
"""
import socket
import struct
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 3240

seq = 0
failures = 0


def check(cond, msg):
    global failures
    print(("ok   " if cond else "ÉCHEC ") + msg)
    if not cond:
        failures += 1


def recv_exact(s, n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise EOFError("connexion fermée")
        b += c
    return b


def submit(s, ep, direction, length, setup=b"\0" * 8, data=b"", iso=None):
    global seq
    seq += 1
    npk = len(iso) if iso else 0
    hdr = struct.pack(">IIIII", 1, seq, 0x10001, direction, ep)
    hdr += struct.pack(">IiiiI", 0, length, 0, npk, 0) + setup
    body = data
    if iso:
        body += b"".join(struct.pack(">IIII", off, ln, 0, 0) for off, ln in iso)
    s.sendall(hdr + body)
    return seq


def read_ret(s):
    hdr = recv_exact(s, 48)
    cmd, sq, _, _, _ = struct.unpack(">IIIII", hdr[:20])
    if cmd == 4:
        (status,) = struct.unpack(">i", hdr[20:24])
        return {"cmd": cmd, "seq": sq, "status": status}
    status, actual, sf, npk, err = struct.unpack(">iiiii", hdr[20:40])
    return {"cmd": cmd, "seq": sq, "status": status, "actual": actual, "npk": npk, "hdr": hdr}


def read_submit_reply(s, direction, iso=False):
    r = read_ret(s)
    assert r["cmd"] == 3, r
    r["data"] = recv_exact(s, r["actual"]) if direction == 1 and r["actual"] > 0 else b""
    if r["npk"] > 0:
        raw = recv_exact(s, 16 * r["npk"])
        r["iso"] = [struct.unpack(">IIII", raw[i : i + 16]) for i in range(0, len(raw), 16)]
    return r


def control_in(s, req_type, req, value, index, length):
    setup = struct.pack("<BBHHH", req_type, req, value, index, length)
    submit(s, 0, 1, length, setup)
    return read_submit_reply(s, 1)


def control_out(s, req_type, req, value, index, data=b""):
    setup = struct.pack("<BBHHH", req_type, req, value, index, len(data))
    submit(s, 0, 0, len(data), setup, data)
    return read_submit_reply(s, 0)


# 1. Devlist
s = socket.create_connection((HOST, PORT))
s.sendall(struct.pack(">HHI", 0x0111, 0x8005, 0))
ver, code, status = struct.unpack(">HHI", recv_exact(s, 8))
(ndev,) = struct.unpack(">I", recv_exact(s, 4))
dev = recv_exact(s, 312)
busid = dev[256:288].split(b"\0")[0].decode()
vid, pid = struct.unpack(">HH", dev[300:304])
nintf = dev[311]
intfs = recv_exact(s, 4 * nintf)
s.close()
check(ver == 0x0111 and code == 0x0005 and status == 0 and ndev == 1, "OP_REP_DEVLIST")
check(busid == "1-1" and (vid, pid) == (0x054C, 0x0CE6) and nintf == 4, f"périphérique {busid} {vid:04x}:{pid:04x}, {nintf} interfaces")

# 2. Import d'un busid inconnu : refusé
s = socket.create_connection((HOST, PORT))
s.sendall(struct.pack(">HHI", 0x0111, 0x8003, 0) + b"9-9".ljust(32, b"\0"))
_, code, status = struct.unpack(">HHI", recv_exact(s, 8))
check(code == 0x0003 and status != 0, "import d'un busid inconnu refusé")
s.close()

# 3. Import
s = socket.create_connection((HOST, PORT))
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
s.sendall(struct.pack(">HHI", 0x0111, 0x8003, 0) + b"1-1".ljust(32, b"\0"))
_, code, status = struct.unpack(">HHI", recv_exact(s, 8))
dev = recv_exact(s, 312)
speed = struct.unpack(">I", dev[296:300])[0]
check(code == 0x0003 and status == 0 and speed == 3, "OP_REP_IMPORT, haute vitesse")

# 4. Contrôle
r = control_in(s, 0x80, 6, 0x0100, 0, 18)
check(r["status"] == 0 and len(r["data"]) == 18 and r["data"][8:12] == b"\x4c\x05\xe6\x0c", "descripteur de périphérique")
r = control_in(s, 0x80, 6, 0x0200, 0, 9)
total = struct.unpack("<H", r["data"][2:4])[0]
check(total == 0xE3, f"en-tête de configuration (wTotalLength {total})")
r = control_in(s, 0x80, 6, 0x0200, 0, total)
check(len(r["data"]) == total, "configuration complète")
r = control_in(s, 0x80, 6, 0x0302, 0x0409, 255)
check(r["data"][2:].decode("utf-16-le") == "DualSense Wireless Controller", "chaîne produit")
r = control_in(s, 0x80, 6, 0x0303, 0x0409, 255)
check(r["status"] == -32, "chaîne inexistante : STALL")
check(control_out(s, 0x00, 9, 1, 0)["status"] == 0, "SET_CONFIGURATION 1")
r = control_in(s, 0x81, 6, 0x2200, 3, 0x121)
check(r["status"] == 0 and len(r["data"]) == 0x121, f"descripteur de rapport HID ({len(r['data'])} octets)")
for rid, ln in ((0x05, 41), (0x09, 20), (0x20, 64)):
    r = control_in(s, 0xA1, 1, 0x0300 | rid, 3, ln)
    check(r["status"] == 0 and len(r["data"]) == ln and r["data"][0] == rid, f"feature 0x{rid:02x} ({ln} octets)")
r = control_in(s, 0xA1, 0x81, 0x0200, 0x0200, 2)
check(r["status"] == 0 and len(r["data"]) == 2, "UAC GET_CUR volume HP")
check(control_out(s, 0x21, 1, 0x0200, 0x0200, b"\x00\xec")["status"] == 0, "UAC SET_CUR volume HP")
check(control_out(s, 0x01, 11, 1, 1)["status"] == 0, "SET_INTERFACE sortie audio alt 1")

# 5. HID IN : rapports toutes les 4 ms
t0 = time.monotonic()
seqs = []
for _ in range(25):
    submit(s, 4, 1, 64)
    r = read_submit_reply(s, 1)
    seqs.append(r["data"][7])
dt = time.monotonic() - t0
check(len(r["data"]) == 64 and r["data"][0] == 1 and r["data"][8] & 0x0F == 8, "rapport d'entrée 0x01 neutre")
check(0.08 <= dt <= 0.14, f"25 rapports HID en {dt * 1000:.0f} ms (attendu ~100)")
check(all((b - a) & 0xFF == 1 for a, b in zip(seqs, seqs[1:])), "numéro de séquence incrémenté")

# 6. HID OUT
out = bytes([0x02]) + bytes(47)
submit(s, 3, 0, len(out), data=out)
r = read_submit_reply(s, 0)
check(r["status"] == 0 and r["actual"] == 48, "rapport de sortie 0x02")

# 7. Isochrone OUT : 4 URB de 8 paquets de 384 octets, 2 en vol (comme snd-usb-audio)
pkt = 384
iso = [(i * pkt, pkt) for i in range(8)]
frame = struct.pack("<hhhh", 0, 0, 1000, -1000)
data = frame * (pkt * 8 // 8)
t0 = time.monotonic()
submit(s, 1, 0, len(data), data=data, iso=iso)
submit(s, 1, 0, len(data), data=data, iso=iso)
done = 0
for i in range(4):
    r = read_submit_reply(s, 0)
    done += 1
    if i < 2:
        submit(s, 1, 0, len(data), data=data, iso=iso)
dt = time.monotonic() - t0
check(r["status"] == 0 and r["actual"] == 8 * pkt and r["npk"] == 8, "RET_SUBMIT isochrone (longueur, paquets)")
check(all(d[2] == pkt for d in r["iso"]), "actual_length par paquet")
check(0.028 <= dt <= 0.045, f"4 URB de 8 ms rendus en {dt * 1000:.1f} ms (attendu ~32)")

# 8. Isochrone IN (micro)
check(control_out(s, 0x01, 11, 1, 2)["status"] == 0, "SET_INTERFACE entrée audio alt 1")
iso = [(i * 196, 196) for i in range(4)]
submit(s, 2, 1, 4 * 196, iso=iso)
r = read_submit_reply(s, 1)
check(r["actual"] == 4 * 192 and len(r["data"]) == 4 * 192 and all(d[2] == 192 for d in r["iso"]), "isochrone IN : 4 paquets de 192 octets compactés")

# 9. UNLINK d'un HID IN en attente (laissé sans réponse : on en soumet deux, la 1re part en 4 ms)
time.sleep(0.01)
a = submit(s, 4, 1, 64)
b = submit(s, 4, 1, 64)
seq += 1
s.sendall(struct.pack(">IIIII", 2, seq, 0x10001, 0, 4) + struct.pack(">I", b) + bytes(24))
got = {}
for _ in range(2):
    r = read_ret(s)
    if r["cmd"] == 3:
        recv_exact(s, r["actual"])
    got[r["cmd"]] = r
check(got.get(3, {}).get("seq") == a, "le premier HID IN est rendu")
check(got.get(4, {}).get("status") == -104, "UNLINK du second : -ECONNRESET")

s.close()
print(f"\n{failures} échec(s)")
sys.exit(1 if failures else 0)
