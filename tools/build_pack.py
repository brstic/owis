#!/usr/bin/env python3
"""Build a PorscheTool data pack (data/<PLATFORM>.json) from a local PIWIS project directory.

Pack format (version 1), compact JSON:
  {"v":1, "platform":"G1", "title":"...", "str":[strings], "dops":[...], "mods":[...]}
  str  : every display string once; everything else refers to strings by index.
  dops : simple   ["s", base, bits, lentype, msb_first, compu, [[lo,hi,n0,n1,den,text]...], unit, mask]
         struct   ["t", [[name, byte, bit, dop|-1, const|null], ...]]
         field    ["f", item_dop]                       (repeated structure up to the end of the message)
  mods : {"k":key, "n":name, "ids":[[req,resp],...], "p":proto, "svc":[...], "dtc":[[code,bits,text],...]}
  svc  : [name, category, semantic, sess_and_flags, request params, response params]
         category: L live data, I identification, A actuator test, R routine, C coding/adaptation read,
                   W coding/adaptation write, O other
         sess_and_flags: low byte = session bitmask; bit 8 = needs Codierung security unlock
  param: [name, byte, bit, bits, const|null, dop|-1]
Strings are English (tools/i18n/*.json, keyed by djb2 of the source text). With --lang de the source texts
are kept (private use only - never publish such a pack).

Usage: python3 -I build_pack.py <PIWIS project dir> <PLATFORM> <out.json> [--lang de]
"""
import sys, os, re, json, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import odxdb as ox

HERE = os.path.dirname(os.path.abspath(__file__))
SESS = {"Default": 1, "Standard": 1, "Extended": 2, "Diagnose": 2, "EndOfLine": 4, "Development": 8, "Programming": 16}
CATN = {"identical": 0, "linear": 1, "scale-linear": 2, "texttable": 3, "compucode": 4, "tab-intp": 5, "rat-func": 6,
        "scale-rat-func": 7}
BASEN = {v: k for k, v in ox.BASE.items()}
LT = {"standard": 0, "minmax": 1, "leading": 2}

def load_tr():
    tr = {}
    d = os.path.join(HERE, "i18n")
    if os.path.isdir(d):
        files = [f for f in os.listdir(d) if f.endswith(".json") and not f.startswith("untranslated")]
        for f in sorted(files, key=lambda f: (not f.startswith("mt_"), f)):     # machine first, hand-made wins
            tr.update(json.load(open(os.path.join(d, f), encoding="utf-8")))
    return tr

class Pack:
    def __init__(self, lang):
        self.lang = lang; self.tr = load_tr(); self.strs = []; self.si = {}; self.dops = []; self.di = {}
        self.missing = set()

    def S(self, text, fallback=None):
        text = (text or "").strip()
        if not text: return -1
        if self.lang != "de":
            en = self.tr.get("%08x" % ox.djb2(text))
            if en is None and re.fullmatch(r"[\x20-\x7e]*", text) and not re.search(r"[A-Za-z]{3,}", text): en = text
            if en is None:
                self.missing.add(text); en = fallback if fallback is not None else ""
            text = en
        if text not in self.si: self.si[text] = len(self.strs); self.strs.append(text)
        return self.si[text]

    def U(self, unit):
        """units are symbols, not translated: 'V (Volt)' -> 'V'"""
        u = re.sub(r"\s*\(.*\)\s*$", "", (unit or "").strip())
        if not u: return -1
        if u not in self.si: self.si[u] = len(self.strs); self.strs.append(u)
        return self.si[u]

    def dop(self, d):
        if not isinstance(d, dict) or d.get("type") in (None, "unresolved"): return -1
        key = json.dumps(d, sort_keys=True, ensure_ascii=False)
        if key in self.di: return self.di[key]
        if d["type"] == "simple":
            sc = []
            for s in d.get("scales", []):
                num = s.get("num", [])
                sc.append([s.get("lo"), s.get("hi"), num[0] if len(num) > 0 else None, num[1] if len(num) > 1 else None,
                           s.get("den", 1.0), self.S(s.get("text"), s.get("text"))])
            v = ["s", BASEN.get(d.get("base"), 1), d.get("bits", 0), LT.get(d.get("lentype"), 0),
                 1 if d.get("msb_first", True) else 0, CATN.get(d.get("compu"), 0), sc, self.U(d.get("unit")),
                 d.get("mask")]
        elif d["type"] == "struct":
            v = ["t", [[self.S(f.get("ln") or f.get("sn"), f.get("sn")), f["byte"], f["bit"],
                        self.dop(f.get("dop")), f.get("const")] for f in d.get("fields", [])]]
        elif d["type"] == "field":
            v = ["f", self.dop(d.get("item"))]
        else: return -1
        self.di[key] = len(self.dops); self.dops.append(v)
        return self.di[key]

    def params(self, ps):
        out = []
        for p in ps or []:
            out.append([self.S(p.get("text") or p.get("name"), p.get("name")), p["byte"], p["bit"], p.get("bits", 0),
                        p.get("const"), self.dop(p.get("dop"))])
        return out

def topology_names(proj):
    """base variant -> (logical link short name, long name) from Allgemein/*.odx-v"""
    out = {}
    d = os.path.join(proj, "Allgemein")
    if not os.path.isdir(d): return out
    for f in os.listdir(d):
        if not f.endswith(".odx-v"): continue
        x = open(os.path.join(d, f), encoding="latin1").read()
        for m in re.finditer(r"<LOGICAL-LINK .*?</LOGICAL-LINK>", x, re.S):
            b = m.group(0)
            sn = re.search(r"<SHORT-NAME>(.*?)</SHORT-NAME>", b); ln = re.search(r"<LONG-NAME[^>]*>(.*?)</LONG-NAME>", b)
            bv = re.search(r'BASE-VARIANT-REF[^>]*ID-REF="([^"]+)"', b)
            if bv and sn: out.setdefault(bv.group(1), (sn.group(1), ln.group(1) if ln else sn.group(1)))
    return out

def pair_ids(ids):
    out = []
    for q in ids.get("req", []):
        for s in ids.get("resp", []):
            if s - q in (0x6A, 0x08, 0x10, 0x100, 0x40, 0x80, 0x20000): out.append((q, s))
    out.sort(key=lambda p: (p[0] > 0x7FF, not (0x700 <= p[0] <= 0x7FF), p[0]))   # 0x7xx, other 11-bit, 29-bit
    return out

# Functions PorscheTool deliberately does not offer (anti-theft, odometer, crash data, high-voltage safety,
# flashing / memory access). Matched on the source name and on the request service id.
EXCLUDE_RE = re.compile(r"(?i)(Wegfahrsperre|Immobili|WFS|Key.?Learn|Schl(ue|ü)ssel.*(lern|anlern|programm)|KESSY|Kryptolog|"
                        r"Komponentenschutz|Component.?Protect|Gesamtwegstrecke|Kilometerstand.*(schreib|setz|Write)|Odometer.*Write|"
                        r"Wegstrecke.*(vorstellen|zur(ue|ü)ck)|Crash|Unfall|Verriegelung|Entriegelung|"
                        r"High_?Voltage_?System_?Deactiv|Isolation.*deaktiv|HV.*(Deaktiv|abschalt)|"
                        r"Erase.?Memory|Flash|Programmier(ung|session|modus)|Programming|Bootloader|Download|Upload|"
                        r"Memory.?(Read|Write)|Transfer.?Data|Security.?Access|SecurAcces|Seed|Fingerprint|Kodiercontainer)")
EXCLUDE_SID = {0x23, 0x27, 0x28, 0x29, 0x34, 0x35, 0x36, 0x37, 0x3D, 0x83, 0x84, 0x85, 0x86, 0x87, 0x10, 0x11, 0x3E,
               0x14, 0x18, 0x19, 0x17, 0x12, 0x13}

SUFFIX_RE = re.compile(r"\s+(Read Data By Identifier|ReadDataByIdentifier|Write Data By Identifier|Request Routine Results|"
                       r"RequestRoutineResults?|Start Routine|StartRoutine|Stop Routine|StopRoutine|Short Term Adjustment|"
                       r"Return Control To E[Cc][Uu]|Freeze Current State|ReadDocumentation|WriteDocumentation|Read Read|Read|Write)$")

def svc_name(ln):
    """service long name without the standard ODX service suffix (the category says what it is)"""
    n = (ln or "").strip()
    for _ in range(2): n = SUFFIX_RE.sub("", n)
    return n or (ln or "")

def category(sem, cls, sid):
    """L live data, I identification, A actuator test, R routine/basic setting, C adaptation/coding read,
    W adaptation/coding write"""
    sem = (sem or "").upper(); cls = (cls or "")
    if sid in (0x2F, 0x30) or "IOCONTROL" in sem: return "A"
    if sid in (0x31, 0x32, 0x33) or "ROUTINE" in sem: return "R"
    if "WRITE" in sem or sid in (0x2E, 0x3B): return "W"
    if "IDENTIFICATION" in sem or sid == 0x1A: return "I"
    if "CURRENTDATA" in sem or cls == "Measurement": return "L"
    if "STOREDDATA" in sem or "Coding" in cls: return "C"
    if sid in (0x22, 0x21): return "L"
    return "O"

def code_key(disp):
    if re.fullmatch(r"[PCBU][0-9A-F]{4}", disp): return 16, ("PCBU".index(disp[0]) << 14) | int(disp[1:], 16)
    return 24, int(disp, 16)

def build(proj, platform, title, lang="en", progress=print):
    pk = Pack(lang); topo = topology_names(proj)
    groups = collections.OrderedDict()                 # CAN pair -> module
    excluded = [0]
    nocan = []
    for f in ox.ecu_files(proj):
        name = f[6:-6]
        e = ox.Ecu(proj, f)
        ids = pair_ids(e.can_ids())
        svcs = e.services(); dtcs = e.dtcs()
        proto = 1 if any(p.get("const") == 0x19 and p["byte"] == 0 for s in svcs for p in s["request"]) else \
                2 if any(p.get("const") == 0x18 and p["byte"] == 0 for s in svcs for p in s["request"]) else 0
        pr = ids[0] if ids else None
        if pr and pr in groups: m = groups[pr]
        else:
            ln = topo.get(name, (name, name))[1]
            m = {"k": name, "n": pk.S(ln, name), "ids": [list(p) for p in ids[:3]], "p": proto, "svc": [], "dtc": [],
                 "_seen": set(), "_dtcseen": set(), "dbs": []}
            if pr: groups[pr] = m
            else: nocan.append(m)
        m["dbs"].append(name)
        for s in svcs:
            if s["sn"] in m["_seen"]: continue
            m["_seen"].add(s["sn"])
            sid = s["request"][0].get("const") if s["request"] else None
            if sid in EXCLUDE_SID or EXCLUDE_RE.search(s["sn"] + " " + s["ln"]):
                excluded[0] += 1; continue
            mask = 0
            for x in s["sessions"]: mask |= SESS.get(x, 0)
            sec = 1 if any("Codier" in x or "CODIER" in x for x in s.get("security", [])) else 0
            m["svc"].append([pk.S(svc_name(s["ln"]), s["sn"]), category(s.get("semantic"), s.get("class"), sid),
                             s.get("semantic", ""), (mask or 1) | (sec << 8), pk.params(s["request"]), pk.params(s.get("response"))])
        for disp, txt in sorted(dtcs.items()):
            bits, code = code_key(disp)
            if (bits, code) in m["_dtcseen"]: continue
            m["_dtcseen"].add((bits, code)); m["dtc"].append([code, bits, pk.S(txt)])
        progress("  %-34s %s svc %4d dtc %4d" % (name, " ".join("%03X/%03X" % p for p in ids[:2]) or "no CAN", len(svcs), len(dtcs)))
    mods = list(groups.values()) + nocan
    for m in mods: m.pop("_seen"); m.pop("_dtcseen")
    progress("  excluded functions (anti-theft, odometer, crash, HV safety, flashing, raw protocol): %d" % excluded[0])
    return {"v": 1, "platform": platform, "title": title, "lang": lang, "str": pk.strs, "dops": pk.dops, "mods": mods}, pk.missing

if __name__ == "__main__":
    proj, plat, out = sys.argv[1], sys.argv[2], sys.argv[3]
    lang = "de" if "--lang" in sys.argv and sys.argv[sys.argv.index("--lang") + 1] == "de" else "en"
    title = sys.argv[sys.argv.index("--title") + 1] if "--title" in sys.argv else plat
    pack, missing = build(proj, plat, title, lang)
    json.dump(pack, open(out, "w", encoding="utf-8"), ensure_ascii=False, separators=(",", ":"))
    if missing:
        json.dump(sorted(missing), open(os.path.join(HERE, "i18n", "untranslated_%s.json" % plat), "w", encoding="utf-8"),
                  ensure_ascii=False, indent=0)
    print("%s: modules %d, services %d, dops %d, strings %d, untranslated %d, %.1f MB" % (
        plat, len(pack["mods"]), sum(len(m["svc"]) for m in pack["mods"]), len(pack["dops"]), len(pack["str"]),
        len(missing), os.path.getsize(out) / 1e6))
