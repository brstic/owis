#!/usr/bin/env python3
"""Generate the local Codierung key file that OWIS loads to unlock coding writes.

Reads the local seed/key catalogue (and the dumped rotate tables) produced from your own PIWIS
install and writes a single binary `owis.keys` next to the executable. This file holds the
unlock constants: it is LOCAL ONLY and must never be committed or published (it is in .gitignore).

Without this file the app still reads faults, live data, runs routines and actuator tests; only
coding writes (0x2E) and the few actuator tests that need SecurityAccess are disabled, with a clear
message. Build it with your own catalogue:

    python3 tools/build_keyfile.py ~/Projects/porsche/seedkey  [out.keys]

Binary format (little-endian):
    magic   "PTKEY1\\0"            (7 bytes)
    u16     count
    per entry:
        u8   nameLen,  name[nameLen]      (ASCII algorithm short-name)
        u8   fam                          (1 = keyed-multiply, 2 = rotate table)
        u8   nb                           (seed/key length in bytes: 2, 3 or 4)
        fam 1 (kmul):  u32 K, u32 A, u32 B      key = ((seed ^ K) * A + B) mod 2^(8*nb)
        fam 2 (table): u32 nbytes, nbytes       key table indexed by seed value, nb bytes per key
Array-hash algorithms are not emitted yet (not ported); modules that need them report "no key".
"""
import sys, os, json, struct, glob

FAM_KMUL, FAM_TABLE, FAM_BMROT, FAM_DIA, FAM_LFSR, FAM_KOMBI, FAM_RSR = 1, 2, 3, 4, 5, 6, 7


def load_table(seedkey_dir, name, nb):
    """Return a bytes buffer: for every seed 0..2^(8*nb)-1, the nb-byte key (big-endian)."""
    for cand in (name + ".txt",):
        p = os.path.join(seedkey_dir, "full_tables", cand)
        if os.path.exists(p):
            break
    else:
        return None
    size = 1 << (8 * nb)
    buf = bytearray(size * nb)
    seen = 0
    with open(p) as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            a, b = line.split()
            seed = int(a, 16); key = int(b, 16)
            if seed < size:
                buf[seed * nb:(seed + 1) * nb] = key.to_bytes(nb, "big")
                seen += 1
    if seen < size:
        print("  ! table %s: %d/%d entries present" % (name, seen, size), file=sys.stderr)
    return bytes(buf)


def main():
    seedkey_dir = os.path.expanduser(sys.argv[1]) if len(sys.argv) > 1 else os.path.expanduser("~/Projects/porsche/seedkey")
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "owis.keys")
    cat = json.load(open(os.path.join(seedkey_dir, "seedkey_catalog.json")))

    entries = []
    skipped = []
    for name, v in sorted(cat.items()):
        fam, nb = v.get("fam"), int(v.get("nb", 4))
        if fam == "kmul":
            body = struct.pack("<III", v["K"] & 0xFFFFFFFF, v["A"] & 0xFFFFFFFF, v["B"] & 0xFFFFFFFF)
            entries.append((name, FAM_KMUL, nb, body))
        elif fam == "table":
            tbl = load_table(seedkey_dir, name, nb)
            if tbl is None:
                skipped.append((name, "no table dump"))
                continue
            entries.append((name, FAM_TABLE, nb, struct.pack("<I", len(tbl)) + tbl))
        else:  # array - emitted from array_ported.json below if ported
            skipped.append((name, "array-hash not ported"))

    # ported array-hash algorithms (verified against their own seed->key vectors before shipping)
    ap_path = os.path.join(seedkey_dir, "array_ported.json")
    ported = json.load(open(ap_path)) if os.path.exists(ap_path) else {}
    skipped = [s for s in skipped if s[0] not in ported]
    for name, v in sorted(ported.items()):
        nb = int(v.get("nb", 4)); fam = v["fam"]
        if fam == "kmul":
            entries.append((name, FAM_KMUL, nb, struct.pack("<III", v["K"] & 0xFFFFFFFF, v["A"] & 0xFFFFFFFF, v["B"] & 0xFFFFFFFF)))
        elif fam == "bmrot":
            body = struct.pack("<I", v["rotleft"] & 0xFFFFFFFF)
            body += b"".join(struct.pack("<I", x & 0xFFFFFFFF) for x in v["rb"])
            body += b"".join(struct.pack("<I", x & 0xFFFFFFFF) for x in v["cb"])
            entries.append((name, FAM_BMROT, nb, body))
        elif fam == "dia":
            def blob(hexs): b = bytes.fromhex(hexs); return struct.pack("<B", len(b)) + b
            body = blob(v["init"]) + blob(v["xor"]) + blob(v["fixed"]) + struct.pack("<B", v["challenge"])
            entries.append((name, FAM_DIA, nb, body))
        elif fam == "lfsr":
            entries.append((name, FAM_LFSR, nb, struct.pack("<IB", v["const"] & 0xFFFFFFFF, v["rounds"] & 0xFF)))
        elif fam == "kombi":
            entries.append((name, FAM_KOMBI, nb, b""))
        elif fam == "rsr":
            entries.append((name, FAM_RSR, nb, struct.pack("<IIB", v["parm1"] & 0xFFFFFFFF, v["parmop"] & 0xFFFFFFFF, v["rounds"] & 0xFF)))

    with open(out, "wb") as fh:
        fh.write(b"PTKEY1\0")
        fh.write(struct.pack("<H", len(entries)))
        for name, fam, nb, body in entries:
            nb_name = name.encode("ascii")
            fh.write(struct.pack("<B", len(nb_name)) + nb_name)
            fh.write(struct.pack("<BB", fam, nb))
            fh.write(body)

    cnt = {FAM_KMUL: 0, FAM_TABLE: 0, FAM_BMROT: 0, FAM_DIA: 0, FAM_LFSR: 0, FAM_KOMBI: 0, FAM_RSR: 0}
    for e in entries: cnt[e[1]] += 1
    print("wrote %s: %d algorithms (%d kmul, %d table, %d bmrot, %d dia, %d lfsr, %d kombi, %d rsr), %d skipped, %d bytes"
          % (os.path.relpath(out), len(entries), cnt[FAM_KMUL], cnt[FAM_TABLE], cnt[FAM_BMROT], cnt[FAM_DIA],
             cnt[FAM_LFSR], cnt[FAM_KOMBI], cnt[FAM_RSR], len(skipped), os.path.getsize(out)))
    if skipped:
        print("skipped:", ", ".join("%s (%s)" % s for s in skipped[:30]))


if __name__ == "__main__":
    main()
