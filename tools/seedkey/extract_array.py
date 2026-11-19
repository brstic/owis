#!/usr/bin/env python3
"""Extract and verify the ported array-hash seed/key algorithms from the Java reflection dump
(Params.java output), and write array_ported.json (parameters) + array_vectors.json (sample
seed/key vectors for the C++ cross-check). LOCAL ONLY - holds unlock constants, never committed.

Families produced:
  kmul   key = ((seed^K)*A+B) mod 2^32            (kKrypto jars, mis-tagged 'array' by derive.py)
  bmrot  rotate-left/right + conjunction          (bm_rotate jars: ACC3, BDE_COD, HR3, ...)
  dia    24/32-bit rotate LFSR bit-hash + packing  (Dia/Fixed_bytes jars: EPB and variants)

Usage: python3 extract_array.py params.txt [out_dir]
"""
import sys, os, json


def parse(path):
    jars = {}; cur = None
    for line in open(path):
        t = line.rstrip("\n").split("\t")
        if t[0] == "JAR": cur = t[1][:-4]; jars[cur] = {"I": {}, "B": {}, "V": [], "nb": 4}
        elif t[0] == "I": jars[cur]["I"][t[1]] = int(t[2])
        elif t[0] == "B": jars[cur]["B"][t[1]] = t[2]
        elif t[0] == "NB": jars[cur]["nb"] = int(t[1])
        elif t[0] == "V": jars[cur]["V"].append((t[1], t[2]))
    return jars


def kmul(seedhex, K, A, B, nb):
    s = int(seedhex, 16); m = (1 << (8 * nb)) - 1
    return format(((((s ^ K) & m) * (A & m) + (B & m)) & m), "0%dX" % (2 * nb))


def rol(x, n): n &= 31; return ((x << n) | (x >> (32 - n))) & 0xFFFFFFFF if n else x
def ror(x, n): n &= 31; return ((x >> n) | (x << (32 - n))) & 0xFFFFFFFF if n else x


def bmrot(seedhex, P):
    v = int(seedhex, 16) & 0xFFFFFFFF
    rc = (1 if v & P["rb"][0] else 0) + (2 if v & P["rb"][1] else 0) + (4 if v & P["rb"][2] else 0) + (8 if v & P["rb"][3] else 0)
    op = (1 if v & P["cb"][0] else 0) + (2 if v & P["cb"][1] else 0)
    r = rol(v, rc) if (v & P["rotleft"]) else ror(v, rc)
    o = {0: v & r, 1: v | r, 2: v ^ r, 3: (~v) & 0xFFFFFFFF}.get(op, 0)
    return format(o, "08X")


def dia(seedhex, P):
    seed = bytes.fromhex(seedhex); init = bytes.fromhex(P["init"]); xm = bytes.fromhex(P["xor"]); fx = bytes.fromhex(P["fixed"])
    cb = len(init); seed_used = P["challenge"] - P["fixedlen"]
    msg = bytearray(seed[:seed_used]) + bytearray(fx); c = bytearray(init)
    for bI in range(len(msg)):
        for bit in range(8):
            t6 = 0x80 if (c[0] & 1) else 0
            for j in range(cb - 1):
                c[j] = (c[j] >> 1) & 0x7F
                if c[j + 1] & 1: c[j] |= 0x80
            c[cb - 1] = (c[cb - 1] >> 1) & 0x7F; c[cb - 1] |= t6
            if msg[bI] & (1 << bit): c[cb - 1] ^= 0x80
            if c[cb - 1] & 0x80:
                for j in range(cb): c[j] = (c[j] ^ xm[j]) & 0xFF
    if cb == 3:
        k = [((c[1] & 0x0F) << 4) | ((c[0] & 0xF0) >> 4), (c[1] & 0xF0) | ((c[2] & 0xF0) >> 4), ((c[0] & 0x0F) << 4) | (c[2] & 0x0F), 0]
    else:
        k = [((c[3] & 0x0F) << 4) | (c[3] & 0xF0), ((c[1] & 0x0F) << 4) | ((c[0] & 0xF0) >> 4), (c[1] & 0xF0) | ((c[2] & 0xF0) >> 4), ((c[0] & 0x0F) << 4) | (c[2] & 0x0F)]
    return bytes(k).hex().upper()


def notseed(seedhex):
    nb = len(seedhex) // 2; m = (1 << (8 * nb)) - 1
    return format((~int(seedhex, 16)) & m, "0%dX" % (2 * nb))


def lfsr(seedhex, const, rounds):
    v = int(seedhex, 16) & 0xFFFFFFFF
    for _ in range(rounds):
        c = v & 0x80000000; v = (v << 1) & 0xFFFFFFFE
        if c: v = (v | 1) ^ const
        v &= 0xFFFFFFFF
    return format(v, "08X")


def rsr_hash(seedhex, parm1, parmop, rounds):
    v = int(seedhex, 16) & 0xFFFFFFFF
    for _ in range(rounds):
        v = (v + parm1) & 0xFFFFFFFF
        for _ in range(2):
            c = v & 1; v = (v >> 1) & 0x7FFFFFFF
            if c: v |= 0x80000000
        if c: v ^= parmop
        v &= 0xFFFFFFFF
    return format(v, "08X")


def kombi(seedhex):
    s = bytes.fromhex(seedhex); o = bytearray(4)
    o[0] = (s[2] + 1) & 0xFF; o[1] = (s[0] + 2) & 0xFF; o[2] = (s[3] + 3) & 0xFF; o[3] = (s[1] + 4) & 0xFF
    for j in range(31):
        i = j % 4
        o[i] = (o[i] + s[(j + 1) % 4]) & 0xFF
        o[i] = (o[i] ^ s[(j + 2) % 4]) & 0xFF
        o[(j + 3) % 4] = (o[(j + 3) % 4] ^ s[j % 4]) & 0xFF
    return bytes(o).hex().upper()


# jars whose key = bitwise NOT of the seed (expressed as keyed-multiply K=0xFFFFFFFF, A=1, B=0)
NOT_JARS = {"ASAM_seedkey_Kompass_Codierung", "ASAM_seedkey_STOPPUHR_Codierung"}
LFSR_ROUNDS = 9
RSR_ROUNDS = 26   # only the SEC_ACCESS_PARM_CODING variant is ported (coding, in scope); download/upload are held


def main():
    jars = parse(sys.argv[1])
    out_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.dirname(os.path.abspath(__file__))
    ported = {}; vectors = {}
    for name, d in jars.items():
        I, B, nb, V = d["I"], d["B"], d["nb"], d["V"]
        entry = None
        if name in NOT_JARS:
            if all(notseed(s) == k.upper() for s, k in V):
                entry = {"fam": "kmul", "nb": nb, "K": 0xFFFFFFFF, "A": 1, "B": 0}
        elif name == "kombi_9x1_codierung":
            if all(kombi(s) == k.upper() for s, k in V):
                entry = {"fam": "kombi", "nb": nb}
        elif "SECURITY_CONST_VALUE" in I:
            const = I["SECURITY_CONST_VALUE"] & 0xFFFFFFFF
            if all(lfsr(s, const, LFSR_ROUNDS) == k.upper() for s, k in V):
                entry = {"fam": "lfsr", "nb": nb, "const": const, "rounds": LFSR_ROUNDS}
        elif "SEC_ACCESS_PARM_1" in I and "SEC_ACCESS_PARM_CODING" in I:
            p1, op = I["SEC_ACCESS_PARM_1"] & 0xFFFFFFFF, I["SEC_ACCESS_PARM_CODING"] & 0xFFFFFFFF
            if all(rsr_hash(s, p1, op, RSR_ROUNDS) == k.upper() for s, k in V):
                entry = {"fam": "rsr", "nb": nb, "parm1": p1, "parmop": op, "rounds": RSR_ROUNDS}
        elif {"kKryptoA", "kKryptoB", "SEC_ECU_KEY"} <= set(I):
            K, A, Bk = I["SEC_ECU_KEY"] & 0xFFFFFFFF, I["kKryptoA"] & 0xFFFFFFFF, I["kKryptoB"] & 0xFFFFFFFF
            if all(kmul(s, K, A, Bk, nb) == k.upper() for s, k in V):
                entry = {"fam": "kmul", "nb": nb, "K": K, "A": A, "B": Bk}
        elif "bm_rotateleft" in I:
            P = {"rotleft": I["bm_rotateleft"] & 0xFFFFFFFF,
                 "rb": [I["bm_rotatebit%d" % i] & 0xFFFFFFFF for i in range(4)],
                 "cb": [I["bm_conjunbit%d" % i] & 0xFFFFFFFF for i in range(2)]}
            if all(bmrot(s, P) == k.upper() for s, k in V):
                entry = {"fam": "bmrot", "nb": nb, **P}
        elif "Fixed_bytes_array" in B:
            P = {"init": B["Calc_bits_init_value"], "xor": B["Xor_mask"], "fixed": B["Fixed_bytes_array"],
                 "challenge": I["Dia_challenge_bytes"], "fixedlen": I["Dia_fixed_bytes"]}
            if all(dia(s, P) == k.upper() for s, k in V):
                entry = {"fam": "dia", "nb": nb, **P}
        if entry:
            ported[name] = entry
            vectors[name] = [[s, k] for s, k in V[:64]]
    json.dump(ported, open(os.path.join(out_dir, "array_ported.json"), "w"), indent=1)
    json.dump(vectors, open(os.path.join(out_dir, "array_vectors.json"), "w"))
    byfam = {}
    for e in ported.values(): byfam[e["fam"]] = byfam.get(e["fam"], 0) + 1
    print("verified & ported:", len(ported), byfam)
    for n in sorted(ported): print("  ", n, ported[n]["fam"])


if __name__ == "__main__":
    main()
