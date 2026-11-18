#!/usr/bin/env python3
"""Reader for the compiled ODX diagnostic databases (*.bv.db) of a local PIWIS installation.

Produces, per control unit, everything a tester needs to talk to it:
  services   : name, semantic (CURRENTDATA, ROUTINE_RESULT, IOCONTROL, ...), allowed sessions,
               request template (constant bytes + variable parameters) and the decoded positive response
               (fields with byte/bit position, length, data type, scaling, unit, text tables);
  dtcs       : fault code -> text;
  comparams  : CAN request / response IDs.

File format (reverse engineered, see docs/DATABASE_FORMAT.md):
  .db  = concatenated zlib streams, one ODX object each, record tag in byte 0:
         0xBE diag service   0x78 request   0x91 positive response   0xAA structure   0x2C simple DOP
         0xAC table row      0x57 DTC       0x31 directory
  strings are referenced by djb2(text) & 0x7FFFFFFF, resolved through A/UStringData.
  PARAM sub-record: 01 <ptype> 00 | 4 | LONG-NAME | SHORT-NAME | 4 | ID | GUID | bitpos u8 | bytepos u32 | kind ...
         ptype a5 = value / coded const, b2 = table key, b0 = table struct
         kind  27 = CODED-CONST (27 <vtype> <u32 value>), 2a = VALUE (... 01 ff <DOP>), 22 = key/struct
stdlib only.
"""
import os, re, struct, zlib, json, collections, functools

def djb2(s):
    h = 5381
    for c in s: h = (h * 33 + ord(c)) & 0xFFFFFFFF
    return h & 0x7FFFFFFF

@functools.lru_cache(None)
def load_strtab(proj_dir):
    m = {}
    for name, enc, cw in (("AStringData", "latin1", 1), ("UStringData", "utf-16le", 2)):
        ip = os.path.join(proj_dir, name + ".idx"); dp = os.path.join(proj_dir, name + ".data")
        for p in (ip, dp):
            if not os.path.exists(p) and os.path.exists(p + ".gz"):
                import gzip; open(p, "wb").write(gzip.open(p + ".gz").read())
        if not (os.path.exists(ip) and os.path.exists(dp)): continue
        idx = open(ip, "rb").read(); dat = open(dp, "rb").read()
        n = struct.unpack_from("<I", idx, 0)[0]
        for k in range(n):
            off, h = struct.unpack_from("<II", idx, 4 + 8 * k)
            if off + 4 > len(dat): continue
            ln = struct.unpack_from("<I", dat, off)[0]
            m[h] = dat[off + 4: off + 4 + ln * cw].decode(enc, "replace")
    return m

def streams(path):
    """-> list of (file offset, decompressed record)"""
    d = open(path, "rb").read(); out = []; i = 0
    while True:
        j = d.find(b"\x78\x9c", i)
        if j < 0: break
        try:
            o = zlib.decompressobj(); r = o.decompress(d[j:]); out.append((j, r))
            i = j + (len(d) - j) - len(o.unused_data)
        except Exception: i = j + 2
    return out

def parse_key(path):
    """.key = B-tree file ("Peter's B Tree"), 8 KB pages. Leaf page: byte 0 = 0, 13-byte header, then entries
    <key len=4> <shared prefix len> <value len> <key suffix> <value>; the value starts with the u32 .db offset.
    Internal pages (byte 0 = 1) and the file-header page are skipped."""
    if not os.path.exists(path): return {}
    k = open(path, "rb").read(); out = {}
    for pg in range(0, len(k), 8192):
        page = k[pg:pg + 8192]
        if len(page) < 16 or page[0] != 0 or b"Peter's B Tree" in page[:64]: continue
        i = 13; prev = b""
        while i + 3 <= len(page):
            kl, sh, vl = page[i], page[i+1], page[i+2]
            if kl != 4 or sh > 4 or vl < 4 or vl > 16: break
            nk = prev[:sh] + page[i+3:i+3+kl-sh]
            if len(nk) != 4 or i + 3 + kl - sh + vl > len(page): break
            out[struct.unpack("<I", nk)[0]] = struct.unpack_from("<I", page, i+3+kl-sh)[0]
            prev = nk; i += 3 + kl - sh + vl
    return out

def u32(r, o): return struct.unpack_from("<I", r, o)[0] if 0 <= o and o + 4 <= len(r) else 0
def f64(r, o): return struct.unpack_from("<d", r, o)[0] if o + 8 <= len(r) else 0.0

BASE = {0: "int", 1: "uint", 2: "float32", 3: "float64", 4: "ascii", 5: "utf8", 6: "unicode2", 7: "bytes"}
CAT = {0: "identical", 1: "linear", 2: "scale-linear", 3: "texttable", 4: "compucode", 5: "tab-intp",
       6: "rat-func", 7: "scale-rat-func"}
SESSIONS = ("Default", "Extended", "EndOfLine", "Development", "Programming", "Standard", "Diagnose")

@functools.lru_cache(None)
def shared_records(proj_dir):
    """Records of the shared layers (functional groups, data libraries) every ECU may reference."""
    out = []
    for f in sorted(os.listdir(proj_dir)):
        if f.startswith("0.0.0@") and (f.endswith(".fg.db") or f.endswith(".sd.db")) and "FaultMemor" not in f \
           and "faultMemory" not in f:
            out += [r for o, r in streams(os.path.join(proj_dir, f))]
    return out

class Ecu:
    def __init__(self, proj_dir, dbfile):
        self.s = load_strtab(proj_dir)
        self.file = dbfile
        st = streams(os.path.join(proj_dir, dbfile))
        self.recs = [r for o, r in st]
        self.own = len(self.recs)
        self.recs += shared_records(proj_dir)
        self.at = {o: r for o, r in st}
        self.km = parse_key(os.path.join(proj_dir, dbfile[:-3] + ".key"))
        self.by2 = collections.defaultdict(list)
        for r in self.recs:
            if len(r) >= 6: self.by2[u32(r, 2)].append(r)
        self.dir = {}                                      # object ID hash -> short-name hash (0x31 directories)
        for r in self.recs:
            if r[0] != 0x31: continue
            for q in range(0, len(r) - 11):
                f = self.n(u32(r, q + 8))
                if f.startswith("0.0.0@") and u32(r, q + 4) not in self.dir and u32(r, q) in self.s:
                    self.dir[u32(r, q + 4)] = u32(r, q)
        self.tabs = collections.defaultdict(list)          # table short-name hash -> [0xAB records]
        for r in self.recs:
            if r[0] == 0xAB and len(r) > 30: self.tabs[u32(r, 22)].append(r)
        self.rows = collections.defaultdict(list)          # row short-name -> [(key, struct dop hash)]
        for r in self.recs:
            if r[0] == 0xAC and len(r) >= 53:
                key = self.n(u32(r, 2)); sn = self.n(u32(r, 17))
                if sn and key:                                 # key: number (ODX 2.0) or key-DOP text (ODX 2.2)
                    sd = u32(r, 49) if r[47:49] == b"\x01\x1b" else 0
                    self.rows[sn].append((int(key) if key.lstrip("-").isdigit() else key, sd, self.n(u32(r, 25))))
        self._dop = {}

    def n(self, h): return self.s.get(h, "")

    # ------------------------------------------------------------- params
    def params(self, r, start=26):
        out = []; i = start
        while i < len(r) - 40:
            if r[i] == 1 and r[i+2] == 0 and r[i+1] in (0xA5, 0xB0, 0xB2, 0xA4, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAF, 0xB1, 0xB3) \
               and u32(r, i+11) in self.s and u32(r, i+7) in self.s:
                p = dict(ptype=r[i+1], ln=self.n(u32(r, i+7)), sn=self.n(u32(r, i+11)),
                         bit=r[i+27], byte=u32(r, i+28))
                k = r[i+32] if i + 32 < len(r) else 0
                p["kind"] = k
                if k == 0x27:                                   # CODED-CONST: 27 <vtype> <value>
                    vt = r[i+33]
                    p["const"] = u32(r, i+34) if vt == 0x0B else (f64(r, i+34) if vt == 0x05 else r[i+34])
                    p["semantic"] = self.n(u32(r, i+42)) if self.n(u32(r, i+42)) else ""
                    j = r.find(b"\x01\x23\x00", i+40, i+80)
                    if j > 0 and r[j+3] == 2: p["bits"] = u32(r, j+4)
                else:
                    p["semantic"] = self.n(u32(r, i+37))
                    j = r.find(b"\x01\xff", i+41, i+60)
                    if j > 0: p["dop"] = u32(r, j+2)
                    if r[i+1] == 0xB2:                          # TABLE-KEY -> table + row
                        j = r.find(b"\x09\x19\x00", i+41, i+70)
                        if j > 0:
                            p["table"] = self.n(u32(r, j+3))
                            if r[j+12] == 1: p["row"] = self.n(u32(r, j+13))
                    if r[i+1] == 0xB0:                          # TABLE-STRUCT -> key param name
                        j = r.find(b"\x10\x17", i+41, i+70)
                        if j > 0: p["keyparam"] = self.n(u32(r, j+2))
                out.append(p); i += 30; continue
            i += 1
        return out

    # ------------------------------------------------------------- DOPs
    def rec(self, h, tags=None):
        # The .key B-tree maps object IDs to .db offsets (exact, also where variants share short names).
        # Sanity check: the record must carry the short name the directory gives for the ID.
        off = self.km.get(h)
        if off is not None:
            r = self.at.get(off); sn = self.dir.get(h)
            if r is not None and (tags is None or r[0] in tags) and (sn is None or struct.pack("<I", sn) in r[:80]):
                return r
        for hh in (h, self.dir.get(h)):
            if hh is None: continue
            for r in self.by2.get(hh, []):
                if tags is None or r[0] in tags: return r
            if (tags is None or 0xAB in tags) and hh in self.tabs: return self.tabs[hh][0]
        return None

    def _tab_has(self, t, row):
        n = u32(t, 26)
        return any(self.n(u32(t, 30 + 16 * k + 12)) == row for k in range(min(n, 4096)) if 30 + 16 * k + 16 <= len(t))

    def dop(self, h, depth=0):
        if h in self._dop: return self._dop[h]
        r = self.rec(h, (0x2C, 0xAA, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B))
        if r is None: d = {"type": "unresolved", "name": self.n(h)}
        elif r[0] == 0xAA: d = self.struct(r, depth)
        elif r[0] == 0x2C: d = self.simple(r)
        else: d = self.field(r, depth)
        self._dop[h] = d
        return d

    def struct(self, r, depth):
        d = {"type": "struct", "name": self.n(u32(r, 2)), "fields": []}
        if depth > 6: return d
        for p in self.params(r, 26):
            f = {"sn": p["sn"], "ln": p["ln"], "byte": p["byte"], "bit": p["bit"]}
            if "const" in p: f["const"] = p["const"]
            if "dop" in p and p["dop"]: f["dop"] = self.dop(p["dop"], depth + 1)
            d["fields"].append(f)
        return d

    def field(self, r, depth):           # END-OF-PDU-FIELD / DYNAMIC fields: repeated basic structure
        d = {"type": "field", "tag": r[0], "name": self.n(u32(r, 2))}
        for q in range(2, min(len(r), 60) - 3):
            v = self.n(u32(r, q))
            if v.startswith("DOP_") or (q > 2 and u32(r, q) in self.by2 and self.rec(u32(r, q), (0xAA,))):
                sub = self.rec(u32(r, q), (0xAA, 0x2C))
                if sub is not None and sub is not r:
                    d["item"] = self.dop(u32(r, q), depth + 1); break
        return d

    def simple(self, r):
        d = {"type": "simple", "name": self.n(u32(r, 2))}
        i = r.find(b"\x01\x0a\x00")
        cat = r[i+3] if i >= 0 else 0
        d["compu"] = CAT.get(cat, str(cat))
        j = r.rfind(b"\x01\x23\x00")                   # coded type follows the compu method
        p = r.find(b"\x01\x3c\x00", j if j > 0 else 0)
        if j > 0:
            lt = r[j+3]
            d["lentype"] = {2: "standard", 1: "minmax", 0: "leading"}.get(lt, str(lt))
            d["bits"] = u32(r, j+4)
            if lt == 2 and r[j+8] == 1:
                n = u32(r, j+9)
                if 0 < n <= 8: d["mask"] = r[j+13:j+13+n].hex()
        if p > 0:
            d["base"] = BASE.get(r[p-4], str(r[p-4])); d["enc"] = r[p-3]; d["msb_first"] = r[p-2] == 1
            d["phys"] = BASE.get(r[p+3], str(r[p+3]))
        if cat in (1, 2, 3, 5, 6, 7):
            d["scales"] = self.scales(r, j if j > 0 else len(r))
        for q in range(0, len(r) - 3):
            v = self.n(u32(r, q))
            if v.startswith("UNIT_"):
                ur = self.rec(u32(r, q), (0x02,))
                disp = self.n(u32(ur, 26)) if ur is not None else ""
                d["unit"] = disp or re.sub(r"^.*_Unit_", "", v)
                break
        return d

    def scales(self, r, end):
        out = []
        marks = [m.start() for m in re.finditer(b"\x01\x19\x00", r[:end])]
        for k, a in enumerate(marks):
            b = marks[k+1] if k + 1 < len(marks) else end
            blk = r[a:b]; sc = {}
            co = [m.start() for m in re.finditer(b"\x01\x14\x00", blk)]
            # coefficient block: 01 14 00 <n u8> <n doubles> (01 <denominator double> | 00)
            # 1st block = physical -> internal, 2nd block = internal -> physical (the one we need)
            if co:
                q = co[1] + 3 if len(co) >= 2 else co[0] + 3; n = blk[q]
                if 0 < n <= 4 and q + 1 + 8 * n <= len(blk):
                    nums = [f64(blk, q + 1 + 8 * t) for t in range(n)]
                    has_den = q + 1 + 8 * n < len(blk) and blk[q + 1 + 8 * n] == 1
                    den = f64(blk, q + 2 + 8 * n) if has_den else 1.0
                    if not (1e-15 < abs(den) < 1e18): den = 1.0
                    if len(co) == 1:                           # only one block: it is phys->internal, invert
                        if n == 2 and nums[1]: nums, den = [-nums[0] / nums[1] * den, den / nums[1]], 1.0
                    sc["num"] = nums; sc["den"] = den
            t = blk.find(b"\x0e")                               # text constant
            while t >= 0:
                v = self.n(u32(blk, t + 1))
                if v: sc["text"] = v; break
                t = blk.find(b"\x0e", t + 1)
            lims = []
            for m in re.finditer(b"\x01\x37\x00", blk):
                q = m.end(); vt = blk[q] if q < len(blk) else 0
                if vt == 0x0B: lims.append(u32(blk, q + 1))
                elif vt == 0x05: lims.append(f64(blk, q + 1))
                elif vt == 0x0A: lims.append(struct.unpack_from("<i", blk, q + 1)[0] if q + 5 <= len(blk) else 0)
            if len(lims) >= 2: sc["lo"], sc["hi"] = lims[-2], lims[-1]
            elif len(lims) == 1: sc["lo"] = sc["hi"] = lims[0]
            if sc: out.append(sc)
        return out

    def table_keydop(self, t):
        """key DOP of a table record (last DOP reference in the record), decoded"""
        for q in range(len(t) - 8, 30, -1):
            v = self.n(u32(t, q))
            if v.startswith("DOP_"): return self.dop(u32(t, q))
        return None

    def key_value(self, key, tables):
        if isinstance(key, int): return key
        for t in tables:
            kd = self.table_keydop(t)
            for sc in (kd or {}).get("scales", []):
                if sc.get("text") == key and isinstance(sc.get("lo"), (int, float)): return int(sc["lo"])
        if not hasattr(self, "_allkeys"):                    # fallback: key texts of every table (incl. libraries)
            self._allkeys = {}
            for lst in self.tabs.values():
                for t in lst:
                    for sc in (self.table_keydop(t) or {}).get("scales", []):
                        if sc.get("text") and isinstance(sc.get("lo"), (int, float)): self._allkeys.setdefault(sc["text"], int(sc["lo"]))
        return self._allkeys.get(key)

    def table_row(self, table, row):
        """(key value, structure DOP hash) of row <row> in table <table>"""
        h = djb2(table); cands = []
        for hh in (h, self.dir.get(h)):
            if hh is not None: cands += self.tabs.get(hh, [])
        t0 = self.rec(h, (0xAB,))
        if t0 is not None and all(t0 is not c for c in cands): cands.insert(0, t0)
        rows = self.rows.get(row, [])
        # ODX 2.0 layout: table entries [key][row ref][file][row short name]
        for t in cands:
            n = u32(t, 26)
            for k in range(min(n, 4096)):
                q = 30 + 16 * k
                if q + 16 > len(t): break
                if self.n(u32(t, q + 12)) == row:
                    key = self.n(u32(t, q))
                    key = int(key) if key.lstrip("-").isdigit() else self.key_value(key, [t])
                    if key is None: break
                    tn = self.n(u32(t, 6)); tid = "__" + tn.rsplit("__", 1)[-1] + "_" if "__" in tn else tn + "_"
                    sds = [sd for kk, sd, rid in rows if sd and tid in rid] or [sd for kk, sd, rid in rows if sd]
                    return key, (sds[0] if sds else 0)
        # ODX 2.2 layout: rows carry a key text resolved through the table's key DOP
        if rows:
            key = self.key_value(rows[0][0], cands)
            sds = [sd for kk, sd, rid in rows if sd]
            if key is not None: return key, (sds[0] if sds else 0)
        return None

    # ------------------------------------------------------------- services
    def services(self):
        out = []; seen = set()
        for r in self.recs[:self.own]:
            if r[0] != 0xBE: continue
            names = [(q, self.n(u32(r, q))) for q in range(30, len(r) - 3) if self.n(u32(r, q))]
            ln = self.n(u32(r, 47)); sn = self.n(u32(r, 51))
            if not sn or sn in seen: continue
            req = pr = None; sem = fc = ""; sess = []; sec = []
            for k, (q, v) in enumerate(names):
                if v.startswith("REQ_") and req is None: req = u32(r, q)
                elif v.startswith("RSP_") and pr is None and k and names[k-1][1].startswith("PR_"): pr = u32(r, names[k-1][0])
                elif v.startswith("FC_"): fc = names[k-1][1] if k else ""
                elif v.startswith("ST_") and k:
                    nxt = names[k+2][1] if k + 2 < len(names) else ""
                    (sec if "SECUR" in nxt.upper() else sess).append(names[k-1][1])
                elif re.fullmatch(r"[A-Z_]{4,}", v) and v not in ("SESSION", "SECURITY", "DATA", "ID") and not sem: sem = v
            if req is None: continue
            rq = self.rec(req, (0x78,)) or self.rec(djb2(self.n(req).replace("REQ_", "RQ_", 1)), (0x78,))
            if rq is None:                                     # REQ_<variant>_RQ_x -> request record named RQ_x
                nm = self.n(req); k = nm.find("_RQ_")
                if k > 0: rq = self.rec(djb2(nm[k+1:]), (0x78,))
            if rq is None: continue
            seen.add(sn)
            svc = {"sn": sn, "ln": ln, "semantic": sem, "class": fc, "sessions": sess, "security": sec,
                   "request": self.layout(rq)}
            pr_rec = self.rec(pr, (0x91,)) if pr else None
            if pr_rec is not None: svc["response"] = self.layout(pr_rec, svc["request"])
            out.append(svc)
        return out

    def layout(self, r, req=None):
        """Flat parameter list of a request / response: const bytes, table key (row key), data fields."""
        ps = self.params(r, 26); out = []; rowsel = None
        if req:
            for p in req:
                if p.get("row"): rowsel = p
        for p in ps:
            e = {"name": p["sn"], "text": p["ln"], "byte": p["byte"], "bit": p["bit"]}
            if "const" in p: e["const"] = p["const"]; e["bits"] = p.get("bits", 8)
            elif p["ptype"] == 0xB2:
                e["kind"] = "key"
                row = p.get("row") or (rowsel or {}).get("row")
                tr = self.table_row(p.get("table", ""), row) if row else None
                if tr:
                    key, sdop = tr; e["const"] = key; e["row"] = row
                    e["bits"] = 16 if key > 0xFF else 8
                    if sdop: e["_rowdop"] = sdop
            elif p["ptype"] == 0xB0:
                e["kind"] = "struct"
                row = (rowsel or {}).get("row")
                key = [x for x in out if x.get("kind") == "key"]
                sd = key[-1].get("_rowdop") if key else None
                if sd: e["dop"] = self.dop(sd)
            elif p.get("dop"):
                e["dop"] = self.dop(p["dop"])
            out.append(e)
        for e in out: e.pop("_rowdop", None)
        # key width: the gap to the next parameter; UDS identifiers (22/2E/2F/31) are always 16 bit
        sid = out[0].get("const") if out else None
        if sid is not None and sid >= 0x40: sid -= 0x40
        for k, e in enumerate(out):
            if e.get("kind") == "key":
                nxt = [x["byte"] for x in out[k+1:] if x["byte"] > e["byte"]]
                if nxt: e["bits"] = 8 * (nxt[0] - e["byte"])
                elif sid in (0x22, 0x2E, 0x2F, 0x31, 0x2C, 0x24): e["bits"] = 16
                else: e["bits"] = 8 if e.get("const", 0) <= 0xFF else 16
        if req is None:
            for e in out:
                if e.get("kind") == "key" and "const" in e: e["row"] = e.get("row")
        return out

    def dtcs(self):
        out = {}
        for r in self.recs[:self.own]:
            if r[0] == 0x57 and len(r) >= 26:
                code = u32(r, 22); disp = self.n(u32(r, 10)); txt = self.n(u32(r, 14))
                if not re.fullmatch(r"[PUBC][0-9A-F]{4}", disp or ""):
                    if not (self.n(u32(r, 6)).startswith("DTC") and txt): continue
                    disp = "%06X" % code
                if txt and disp not in out: out[disp] = txt.strip()
        return out

    def can_ids(self):
        res = collections.defaultdict(set)
        hs = {"req": djb2("CP_CanPhysReqId"), "resp": djb2("CP_CanRespUSDTId")}
        for r in self.recs[:self.own]:
            for k, h in hs.items():
                pk = struct.pack("<I", h); i = r.find(pk)
                while i >= 0:
                    j = r.find(b"\x8b\x0b", i + 4, i + 40)
                    if j > 0: res[k].add(u32(r, j + 2))
                    i = r.find(pk, i + 1)
        return {k: sorted(v) for k, v in res.items()}

def ecu_files(proj_dir):
    return sorted(f for f in os.listdir(proj_dir) if f.startswith("0.0.0@") and f.endswith(".bv.db"))

if __name__ == "__main__":
    import sys
    e = Ecu(sys.argv[1], "0.0.0@%s.bv.db" % sys.argv[2])
    svcs = e.services()
    print(len(svcs), "services")
    for s in svcs:
        if not sys.argv[3:] or re.search(sys.argv[3], s["ln"]):
            print(json.dumps(s, ensure_ascii=False)[:3000])
