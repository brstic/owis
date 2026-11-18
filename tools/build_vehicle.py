#!/usr/bin/env python3
"""Build the module table for one vehicle line from a local PIWIS installation.

For every control unit database (0.0.0@<ECU>.bv.db) of a PIWIS project directory this reads:
  * the physical CAN request / response IDs (ODX comparams CP_CanPhysReqId / CP_CanRespUSDTId),
  * the diagnostic protocol (UDS if the ECU defines ReadDTCInformation 0x19, KWP2000 if 0x18),
  * the fault-code catalogue (numeric code -> text).
It then groups variants that share one CAN address into one "module" and writes a C++ header.

The public header carries English texts only (tools/i18n/dtc_en.json, keyed by djb2 hash of the original
text, so the repository holds no original database texts).
--with-de additionally embeds the original texts for a private build.

Usage: python3 -I build_vehicle.py <PIWIS project dir, e.g. .../Projects-V/G1> <VEHICLE_ID> <out.h> [--with-de]
stdlib only.
"""
import sys, os, re, json, struct, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import piwis_db as pb

# English module names (our wording) and display order.  ECU database name -> group key.
GROUPS = [
    # key            English name                                  member databases (variants)
    ("ENGINE",      "Engine electronics (DME)",                    ["DME_BDE_Continental", "BV_ECM30TFS", "BV_ECM30_GEN2_TNF"]),
    ("PDK",         "PDK transmission",                            ["Getriebesteuerung_PDK"]),
    ("TCM_UDS",     "Transmission (UDS variant)",                  ["BV_TransContrModulUDS"]),
    ("SELECTOR",    "Gear selector",                               ["Waehlhebel"]),
    ("PSM",         "PSM (ABS / stability control)",               ["PSM"]),
    ("EPB",         "Electric parking brake",                      ["EPB"]),
    ("PASM",        "PASM / air suspension",                       ["PASM"]),
    ("PDCC",        "PDCC (active roll stabilisation)",            ["PDCC"]),
    ("STEERING",    "Power steering",                              ["Lenkhilfe"]),
    ("COLUMN",      "Steering column switch",                      ["Lenkstockschalter"]),
    ("TPMS",        "Tyre pressure monitoring",                    ["RDK", "RDK3"]),
    ("AIRBAG",      "Airbag",                                      ["Airbag"]),
    ("GATEWAY",     "Gateway",                                     ["CAN_CAN_Gateway"]),
    ("CLUSTER",     "Instrument cluster",                          ["Kombiinstrument"]),
    ("BCM_F",       "Body control module front",                   ["BCM_vorne"]),
    ("BCM_R",       "Body control module rear",                    ["BCM_hinten"]),
    ("HEADLIGHTS",  "Headlights / beam levelling",                 ["SCHEINWERFER", "SCHEINWERFER_LED", "SWSG_mit_AFS_ALWR"]),
    ("CLIMATE",     "Climate control",                             ["BKE"]),
    ("TAILGATE",    "Tailgate",                                    ["Heckklappe"]),
    ("PARKASSIST",  "Park assist",                                 ["PDC"]),
    ("ACC",         "Adaptive cruise control",                     ["ACC3"]),
    ("FRONTCAM",    "Front camera",                                ["Frontkamera"]),
    ("REARCAM",     "Rear view / top view camera",                 ["Rueckfahrkamera_II", "RealTopview", "Rueckfahrkamera"]),
    ("LANECHANGE",  "Lane change assist",                          ["SWA2", "SWA"]),
    ("CLOCK",       "Sport Chrono / additional instrument",        ["Zusatzinstrument"]),
    ("HEADUNIT",    "Head unit (PCM)",                             ["Head_Unit"]),
    ("AMP",         "Audio amplifier",                             ["Verstaerker"]),
    ("TV",          "TV tuner",                                    ["TVTuner"]),
    ("BATTERY",     "Battery management (hybrid)",                 ["BMS2", "BV_BatteEnergContrModulUDS"]),
    ("CHARGER",     "Charger (hybrid)",                            ["BV_BatteChargContrModulUDS"]),
    ("EMOTOR",      "Electric drive motor (hybrid)",               ["BV_DriveMotorContrModulUDS_A01"]),
    ("ACCOMP",      "Electric A/C compressor (hybrid)",            ["BV_AirCondiComprUDS"]),
    # No CAN address in the database (reached over a sub-bus / VW TP2.0) - listed, not scanned yet:
    ("SEATS",       "Seat control",                                ["Sitzsteuergeraet"]),
    ("DOORS_F",     "Front doors",                                 ["TUER_VORNE"]),
    ("DOORS_R",     "Rear doors",                                  ["TUER_HINTEN"]),
    ("TRAILER",     "Trailer hitch",                               ["Anhaenger"]),
    ("HEATER",      "Auxiliary heater",                            ["Zusatzheizung"]),
    ("DIFFLOCK",    "Rear differential lock",                      ["Quersperre"]),
]

CP = {"req": pb.djb2("CP_CanPhysReqId"), "resp": pb.djb2("CP_CanRespUSDTId")}

def can_ids(path):
    res = collections.defaultdict(set)
    for o, r in pb.streams(path):
        for k, h in CP.items():
            p = struct.pack("<I", h); i = r.find(p)
            while i >= 0:
                j = r.find(b"\x8b\x0b", i+4, i+40)
                if j > 0: res[k].add(struct.unpack_from("<I", r, j+2)[0])
                i = r.find(p, i+1)
    return {k: sorted(v) for k, v in res.items()}

def pair_ids(ids):
    """Match request and response IDs. Porsche/VW 11-bit diagnostics: resp = req + 0x6A (0x7E0 -> 0x7E8 is OBD)."""
    out = []
    for q in ids.get("req", []):
        for s in ids.get("resp", []):
            if s - q in (0x6A, 0x08, 0x10, 0x100): out.append((q, s))
    return out

def code_key(disp):
    """'P1872' -> (16, 0x1872 with SAE letter bits); '0C120A2' hex -> (24, value)."""
    if re.fullmatch(r"[PCBU][0-9A-F]{4}", disp):
        return 16, ("PCBU".index(disp[0]) << 14) | int(disp[1:], 16)
    return 24, int(disp, 16)

TRANSLIT = {"\u00b0": " deg", "\u00b5": "u", "\u2013": "-", "\u2014": "-", "\u2018": "'", "\u2019": "'",
            "\u201c": '"', "\u201d": '"', "\u00e4": "ae", "\u00f6": "oe", "\u00fc": "ue", "\u00df": "ss",
            "\u00c4": "Ae", "\u00d6": "Oe", "\u00dc": "Ue", "\u00b2": "2", "\u00b3": "3", "\u00a0": " "}

def c_str(s):
    s = re.sub(r"\s*[\r\n]+\s*", " / ", s.strip())           # multi-line texts -> one line
    s = "".join(TRANSLIT.get(ch, ch) for ch in s)
    s = s.replace("\\", "\\\\").replace('"', '\\"').encode("ascii", "replace").decode()
    return '"' + s.replace("??", "?\\?") + '"'                  # no trigraphs

def main():
    proj, vid, out = sys.argv[1], sys.argv[2], sys.argv[3]
    with_de = "--with-de" in sys.argv
    here = os.path.dirname(os.path.abspath(__file__))
    tr = {}
    trp = os.path.join(here, "i18n", "dtc_en.json")
    if os.path.exists(trp): tr = json.load(open(trp, encoding="utf-8"))
    dbs = {f[6:-6]: f for f in os.listdir(proj) if f.startswith("0.0.0@") and f.endswith(".bv.db")}
    pb.load_strtab = __import__("functools").lru_cache(None)(pb.load_strtab)

    mods = []; missing_tr = set(); dtc_rows = []; used = set()
    for key, name, members in GROUPS:
        present = [m for m in members if m in dbs]
        if not present: continue
        pairs = []; proto = set(); dtcs = {}
        for m in present:
            mp = [p for p in pair_ids(can_ids(os.path.join(proj, dbs[m]))) if p not in used]
            for p in mp:
                if p not in pairs: pairs.append(p)
            ex = pb.extract(proj, m)
            for nm, s in ex["services"].items():       # protocol only from variants we can reach
                if mp and s["bytes"].startswith("19 02"): proto.add("UDS")
                if mp and s["bytes"].startswith("18 "): proto.add("KWP")
                if not mp and not pairs and s["bytes"].startswith("18 "): proto.add("KWP")
            for disp, texts in ex["dtcs"].items():
                k = code_key(disp)
                if k not in dtcs: dtcs[k] = texts[0].strip()
        # prefer the classic 0x7xx diagnostic pair first
        pairs.sort(key=lambda p: (p[0] < 0x700, p[0]))
        used.update(pairs)
        mods.append(dict(key=key, name=name, pairs=pairs[:2], proto=sorted(proto), dtcs=dtcs, dbs=present))
        for (bits, code), de in sorted(dtcs.items()):
            en = tr.get("%08x" % pb.djb2(de), "")        # keyed by hash: no original texts in the repo
            if not en: missing_tr.add(de)
            dtc_rows.append((key, bits, code, en, de))

    L = []
    L.append("// Generated by tools/build_vehicle.py from a local PIWIS installation - do not edit.")
    L.append("// Vehicle: %s.  Module names and English texts: porsche-open-diag project." % vid)
    L.append("#pragma once")
    L.append("#include \"diag_core.h\"")
    L.append("namespace veh_%s {" % vid)
    L.append("static const DtcText DTCS[] = {")
    for key, bits, code, en, de in dtc_rows:
        L.append("  {M_%s,%d,0x%06X,%s,%s}," % (key, bits, code, c_str(en), c_str(de) if with_de else '""'))
    L.append("  {-1,0,0,0,0}};")
    L.append("static const Module MODULES[] = {")
    for m in mods:
        p = m["pairs"] + [(0, 0)] * (2 - len(m["pairs"]))
        pr = "PROTO_UDS" if m["proto"] == ["UDS"] else "PROTO_KWP" if m["proto"] == ["KWP"] else "PROTO_AUTO"
        L.append("  {M_%s,%s,0x%03X,0x%03X,0x%03X,0x%03X,%s}," % (m["key"], c_str(m["name"]), p[0][0], p[0][1], p[1][0], p[1][1], pr))
    L.append("  {-1,0,0,0,0,0,0}};")
    L.append("}")
    enum = "enum ModuleKey_%s { %s };" % (vid, ", ".join("M_%s = %d" % (m["key"], i) for i, m in enumerate(mods)))
    L.insert(5, enum)
    open(out, "w").write("\n".join(L) + "\n")
    print("modules %d, with CAN address %d, fault codes %d, untranslated %d" %
          (len(mods), sum(1 for m in mods if m["pairs"]), len(dtc_rows), len(missing_tr)))
    for m in mods:
        print("  %-11s %-40s %s %s %d codes" % (m["key"], m["name"], " ".join("%03X/%03X" % p for p in m["pairs"]) or "-------",
                                             "/".join(m["proto"]), len(m["dtcs"])))
    if missing_tr:
        json.dump(sorted(missing_tr), open(os.path.join(here, "i18n", "untranslated_%s.json" % vid), "w", encoding="utf-8"),
                  ensure_ascii=False, indent=0)

if __name__ == "__main__":
    main()
