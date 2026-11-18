#!/usr/bin/env python3
"""PIWIS gearbox ODX database -> byte-level KWP2000 service data (level 1, part 2).

Decodes the binary ODX object records of a project's 0.0.0@Getriebesteuerung_PDK.bv.db:
  * requests (record tag 0x78): params with byte position, CODED-CONST value (SID / sub-function)
    and TABLE-KEY params resolved through table rows (tag 0xAC, key stored as decimal text);
  * table rows -> structure DOPs (tag 0xAA: fields with byte/bit position + DOP);
  * simple DOPs (tag 0x2C): bit length, rational COMPU (phys = (c0 + c1*x) / d), unit, text table;
  * DTCs (tag 0x57): code -> display P-code -> German text.
Format facts (reverse engineered here, validated on G1 against firmware/car facts):
  .db  = concatenated zlib streams, one object each.  Strings: hash = djb2(text) & 0x7FFFFFFF.
  .key = "Peter's B Tree" (8 KB pages, prefix-compressed keys) mapping object-name hash -> .db offset.
  PARAM sub-record: 01 <type> 00 | desc | LONG-NAME | SHORT-NAME | tmplA | tmplB | GUID | bitpos | bytepos ...
                    CODED-CONST value = '27 0b <value>' after the header; DOP ref = u32 at +47.
stdlib only.  Usage: python3 -I piwis_bytes.py <proj_dir> <out.json>
"""
import struct, zlib, os, re, json, sys, collections

def djb2(s):
    h = 5381
    for c in s: h = (h*33 + ord(c)) & 0xFFFFFFFF
    return h & 0x7FFFFFFF

def load_strtab(proj_dir):
    m = {}
    for name, enc, cw in (("AStringData","latin1",1), ("UStringData","utf-16le",2)):
        ip = os.path.join(proj_dir, name+".idx"); dp = os.path.join(proj_dir, name+".data")
        if not (os.path.exists(ip) and os.path.exists(dp)): continue
        idx = open(ip,"rb").read(); dat = open(dp,"rb").read()
        n = struct.unpack_from("<I", idx, 0)[0]
        for k in range(n):
            off, h = struct.unpack_from("<II", idx, 4+8*k)
            if off+4 > len(dat): continue
            ln = struct.unpack_from("<I", dat, off)[0]
            m[h] = dat[off+4: off+4+ln*cw].decode(enc, "replace")
    return m

def streams(path):
    d = open(path,"rb").read(); out = []; i = 0
    while True:
        j = d.find(b"\x78\x9c", i)
        if j < 0: break
        try:
            o = zlib.decompressobj(); r = o.decompress(d[j:]); out.append((j, r))
            i = j + (len(d)-j) - len(o.unused_data)
        except Exception: i = j+2
    return out

def parse_key(path):
    k = open(path,"rb").read(); out = {}
    for pg in range(0, len(k), 8192):
        page = k[pg:pg+8192]; i = page.find(b"\x04\x00\x06"); prev = b""
        if i < 0: continue
        while i+3 <= len(page):
            kl, sh, vl = page[i], page[i+1], page[i+2]
            if kl != 4 or sh > 4 or vl not in (4,6,8): break
            nk = prev[:sh] + page[i+3:i+3+kl-sh]
            if len(nk) != 4: break
            val = page[i+3+kl-sh:i+3+kl-sh+vl]
            out[struct.unpack("<I", nk)[0]] = struct.unpack_from("<I", val, 0)[0]
            prev = nk; i += 3+kl-sh+vl
    return out

def u32(r, o): return struct.unpack_from("<I", r, o)[0] if o+4 <= len(r) else 0

class DB:
    def __init__(self, proj_dir, ecu="Getriebesteuerung"):
        self.dir = proj_dir
        dbf = [f for f in os.listdir(proj_dir) if f == "0.0.0@%s.bv.db" % ecu or (f.endswith(".bv.db") and ecu in f)][0]
        self.s = load_strtab(proj_dir)
        self.st = streams(os.path.join(proj_dir, dbf))
        self.km = parse_key(os.path.join(proj_dir, dbf[:-3]+".key"))
        self.offs = {o: i for i, (o, r) in enumerate(self.st)}
        self.by2 = collections.defaultdict(list)
        for i, (o, r) in enumerate(self.st):
            if len(r) >= 6: self.by2[u32(r,2)].append(i)
        self.filehash = djb2(dbf[:-3])                         # e.g. 0.0.0@Getriebesteuerung_PDK.bv
        self.dopdir = {}                                       # DOP id -> short-name hash
        for o, r in self.st:
            if r[0] != 0x31: continue
            for q in range(0, len(r)-11):
                if u32(r,q+8) == self.filehash and self.s.get(u32(r,q+4),"").startswith("DOP_"):
                    self.dopdir[u32(r,q+4)] = u32(r,q)
        self.rows = {}                                         # row short-name -> key (int)
        self.rowstruct = collections.defaultdict(list)         # row short-name -> [struct DOP ids]
        for o, r in self.st:
            if r[0] == 0xAC and len(r) >= 53:
                key = self.s.get(u32(r,2)); sn = self.s.get(u32(r,17))
                if sn and key and key.lstrip("-").isdigit():
                    self.rows[sn] = int(key)
                    self.rowstruct[sn].append(u32(r,49))

    def name(self, h): return self.s.get(h, "")

    def rec_by_dop(self, dopid):
        if dopid in self.km and self.km[dopid] in self.offs: return self.st[self.offs[self.km[dopid]]][1]
        sn = self.dopdir.get(dopid)
        if sn and self.by2.get(sn): return self.st[self.by2[sn][0]][1]
        return None

    def params(self, r):
        out = []; i = 26
        while i < len(r)-40:
            if r[i] == 1 and r[i+2] == 0 and u32(r,i+11) in self.s and u32(r,i+7) in self.s:
                p = dict(type=r[i+1], ln=self.name(u32(r,i+7)), sn=self.name(u32(r,i+11)),
                         bitpos=r[i+27], bytepos=r[i+28], dop=u32(r,i+47))
                j = r.find(b"\x27\x0b", i+29, i+46)
                if j > 0: p["const"] = r[j+2]
                out.append(p); i += 30; continue
            i += 1
        return out

    def dop(self, dopid, depth=0):
        r = self.rec_by_dop(dopid)
        if r is None: return {"dop": self.name(dopid), "unresolved": True}
        d = {"dop": self.name(dopid), "name": self.name(u32(r,2)), "tag": r[0]}
        if r[0] == 0xAA and depth < 3:                        # STRUCTURE
            d["fields"] = [dict(sn=p["sn"], text=p["ln"], byte=p["bytepos"], bit=p["bitpos"],
                                **{k: v for k, v in self.dop(p["dop"], depth+1).items() if k not in ("tag",)})
                           for p in self.params(r)]
            return d
        if r[0] == 0x2C:                                       # simple DOP
            j = r.find(b"\x01\x23\x00")
            if j > 0: d["bits"] = r[j+4]
            blocks = []
            for m in re.finditer(rb"\x01\x14\x00\x02", r):
                q = m.end()
                try:
                    c0, c1 = struct.unpack_from("<dd", r, q)
                    den = struct.unpack_from("<d", r, q+17)[0] if r[q+16] == 1 else 1.0
                    if not (1e-9 < abs(den) < 1e12): den = 1.0       # denominator not stored -> 1
                    blocks.append((c0, c1, den))
                except struct.error: pass
            # block 1 = PHYS->INTERNAL, block 2 = INTERNAL->PHYS (verified: temp x-48, rod x/256, Vref x/50)
            if len(blocks) >= 2:
                c0, c1, den = blocks[1]; d["factor"] = c1/den; d["offset"] = c0/den
            elif len(blocks) == 1:
                c0, c1, den = blocks[0]
                if c1: d["factor"] = den/c1; d["offset"] = -c0/c1; d["compu_uncertain"] = True
            for q in range(0, len(r)-3):
                v = self.name(u32(r,q))
                if v.startswith("UNIT_"): d["unit"] = v.split("_Unit_")[-1]; break
            texts = []
            for q in range(0, len(r)-3):
                v = self.name(u32(r,q))
                if v and not v.startswith(("UNIT_","0.0.0@","DOP_")) and v != d["name"] \
                   and not re.fullmatch(r"[0-9A-F]{32}", v) and (not texts or texts[-1] != v): texts.append(v)
            if texts and "factor" not in d: d["texttable"] = texts
        return d

    def requests(self):
        out = {}
        for o, r in self.st:
            if r[0] != 0x78 or len(r) < 30: continue
            nm = self.name(u32(r,6))
            if not nm or nm in out: continue
            ps = self.params(r); by = []
            rowkey = None
            for q in range(0, len(r)-3):
                v = self.name(u32(r,q))
                if v in self.rows: rowkey = (v, self.rows[v]); break
            for p in sorted(ps, key=lambda p: p["bytepos"]):
                if "const" in p: by.append(dict(byte=p["bytepos"], value=p["const"], name=p["sn"]))
                elif p["type"] == 0xB2 and rowkey: by.append(dict(byte=p["bytepos"], value=rowkey[1], name=p["sn"], row=rowkey[0]))
                else: by.append(dict(byte=p["bytepos"], value=None, name=p["sn"], text=p["ln"]))
            out[nm] = by
        return out

    def dtcs(self):
        out = {}
        for o, r in self.st:
            if r[0] == 0x57 and len(r) >= 26:
                code = u32(r,22); disp = self.name(u32(r,10)); txt = self.name(u32(r,14))
                if not re.fullmatch(r"[PUBC][0-9A-F]{4}", disp or ""):
                    sn = self.name(u32(r,6))                    # UDS-style record: no P-code label,
                    if not (sn.startswith("DTC") and txt): continue   # numeric code at +22, short name DTCxxxx
                    disp = "%06X" % code
                if not txt: continue
                out.setdefault(disp, [])
                if txt.strip() not in [t.strip() for t in out[disp]]: out[disp].append(txt)
        return out

def hexreq(by):
    return " ".join("%02X" % b["value"] if isinstance(b["value"], int) and b["value"] < 256 else
                    ("%04X" % b["value"] if isinstance(b["value"], int) else "<%s>" % b["name"]) for b in by)

def extract(proj_dir, ecu="Getriebesteuerung"):
    db = DB(proj_dir, ecu); req = db.requests()
    services = {nm[3:] if nm.startswith("RQ ") else nm: dict(bytes=hexreq(by), params=by) for nm, by in req.items()}
    routines = {sn: k for sn, k in db.rows.items()}
    # routine result layouts + condition bits, measurement layouts
    layouts = {}
    for sn, dops in db.rowstruct.items():
        for dopid in dops:
            dn = db.name(db.dopdir.get(dopid, 0)) or db.name(dopid)
            rec = db.rec_by_dop(dopid)
            if rec is None or rec[0] != 0xAA: continue
            dd = db.dop(dopid); nm = db.name(u32(rec,2))
            old = layouts.setdefault(sn, {}).get(nm)
            # several variants (base / PDK_4000 / PDKit_2000 ...) share a row name: keep the richest structure
            if old is None or len(dd.get("fields", [])) > len(old.get("fields", [])):
                dd["variant_dop"] = db.name(dopid); layouts[sn][nm] = dd
    sec = sorted(set(v for v in db.s.values() if re.search(r"(?i)ASAM_seedkey|AccessMode|SecurityAccess\.", v)
                     and db.name(djb2(v)) == v))[:20]
    return dict(platform=os.path.basename(proj_dir.rstrip("/")), services=services, table_keys=routines,
                layouts=layouts, dtcs=db.dtcs(), security_strings=sec)

if __name__ == "__main__":
    r = extract(sys.argv[1], sys.argv[3] if len(sys.argv) > 3 else "Getriebesteuerung")
    json.dump(r, open(sys.argv[2], "w"), ensure_ascii=False, indent=1)
    print(r["platform"], "services", len(r["services"]), "table keys", len(r["table_keys"]),
          "layouts", len(r["layouts"]), "dtcs", len(r["dtcs"]))
