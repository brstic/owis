#!/usr/bin/env python3
"""Build a PorscheTool data pack (data/<PLATFORM>.json) from a local PIWIS project directory.

Pack format (version 1), compact JSON:
  {"v":1, "platform":"G1", "title":"...", "str":[strings], "dops":[...], "mods":[...]}
  str  : every display string once; everything else refers to strings by index.
  dops : simple   ["s", base, bits, lentype, msb_first, compu, [[lo,hi,n0,n1,den,text]...], unit, mask]
         struct   ["t", [[name, byte, bit, dop|-1, const|null, const bits], ...]]
         field    ["f", item_dop]                       (repeated structure up to the end of the message)
  mods : {"k":key, "n":name, "ids":[[req,resp],...], "p":proto, "svc":[...], "dtc":[[code,bits,text],...]}
         optional: "sk" coding seed/key algorithm name, "sa" [request-seed sub-function, seed bytes] of the coding
                   security level, "sfd":1 unit uses online authentication (SFD), "link":1 + "kl":[init, 5-baud address, baud] for K-line units
                   (then ids = [[ECU address, tester address]])
  svc  : [name, category, semantic, sess_and_flags, request params, response params]
         category: L live data, I identification, A actuator test, R routine, C coding/adaptation read,
                   W coding/adaptation write, O other, E layout of the fault environment data (UDS 19 06)
  top level, optional: "vin" VIN model types, "pn" part-number prefixes, "jobs" guided jobs (see jobs.py)
         sess_and_flags: low byte = session bitmask; bit 8 = needs Codierung security unlock
  param: [name, byte, bit, bits, const|null, dop|-1]
Strings are English (tools/i18n/*.json, keyed by djb2 of the source text). With --lang de the source texts
are kept (private use only - never publish such a pack).

Usage: python3 -I build_pack.py <PIWIS project dir> <PLATFORM> <out.json> [--lang de]
"""
import sys, os, re, json, collections, glob
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import odxdb as ox
import seedkey_map as skmap
import platforms, jobs as jobdefs

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
        self.src = {}                       # string index -> source text (job rules match the database names)
        self.missing = set()

    def S(self, text, fallback=None):
        text = (text or "").strip()
        if not text: return -1
        src = text
        if self.lang != "de":
            en = self.tr.get("%08x" % ox.djb2(text))
            if en is None and re.fullmatch(r"[\x20-\x7e]*", text) and not re.search(r"[A-Za-z]{3,}", text): en = text
            if en is None:
                self.missing.add(text); en = fallback if fallback is not None else ""
            text = en
        if text not in self.si: self.si[text] = len(self.strs); self.strs.append(text)
        self.src.setdefault(self.si[text], src)
        return self.si[text]

    def L(self, text):
        """literal string of the tool itself (already English, not from the database)"""
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
                        self.dop(f.get("dop")), f.get("const"), f.get("bits", 8)] for f in d.get("fields", [])]]
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

# Readable names for the control units of the older lines (996/986, 997/987, Cayenne 955), whose databases carry
# only technical short names. Matched on the short name without the line prefix.
OLD_NAMES = [(r"^(DME|dme|ME7|MED|med91|EDC17|sdi41|SDI)", "Engine (DME)"), (r"^(ABS|PSM|psm)", "PSM / ABS"), (r"^(Airbag|airbag)", "Airbag"),
             (r"^(Alarmanlage|aws|AWS)", "Alarm system"), (r"^(Klima|klima)", "Climate control"), (r"^(Kombi|kombi)", "Instrument cluster"),
             (r"^PCM1", "PCM 1 (radio/navigation)"), (r"^(RDK|rdk)", "Tyre pressure monitoring"), (r"^(Sitz|sitz)", "Seat"), (r"^(TargaDach|targa)", "Targa roof"),
             (r"^(Tiptronic|tiptrnc)", "Tiptronic transmission"), (r"^(UEB|ueb)", "Central locking / alarm"), (r"^(Verdeck|vdst)", "Convertible top"),
             (r"^(lwr|LWR)", "Headlight range control"), (r"^(pdc|PDC)", "Park assist"), (r"^(gateway|Gateway)", "Gateway"), (r"^bsg", "Onboard power supply"),
             (r"^heck", "Rear-end electronics"), (r"^kessy", "Keyless entry"), (r"^komp", "Compass"), (r"^lenk", "Steering wheel electronics"),
             (r"^lssg", "Transfer case"), (r"^qssg", "Rear differential lock"), (r"^niveau", "Air suspension (levelling)"), (r"^pdcc", "PDCC"),
             (r"^swsg", "Cornering light"), (r"^wisch", "Wipers"), (r"^wzh", "Auxiliary heater"), (r"^grda", "Garage door opener"),
             (r"^ahsg", "Trailer hitch"), (r"^bde", "Engine (DME)"), (r"^bug", "Door electronics"), (r"^tuer", "Door electronics"), (r"^uhr", "Sport Chrono clock"),
             (r"^kurve", "Cornering light"), (r"^pasm", "PASM"), (r"^ptm", "PTM (all-wheel drive)"), (r"^park", "Park assist"), (r"^pas$", "Park assist"),
             (r"^pcm2_", "PCM 2"), (r"^CDR24", "CDR-24 radio")]

# Newer lines name control units after the VW logical link, every word cut to five letters
# (BV_BatteEnergContrModulUDS). Expanded here into readable English, with the PIWIS short name from the
# topology (Common/Topologie/*.xml) in brackets.
VW_WORDS = {"Activ": "Active", "Accel": "Accelerator", "Actua": "Actuator", "Adapt": "Adaptive", "Adjus": "Adjustment", "Aerod": "Aerodynamics",
    "Air": "Air", "Airba": "Airbag", "Allra": "All-wheel", "All": "All", "Wheel": "Wheel", "Analo": "Analog", "Anten": "Antenna", "Assem": "Assembly",
    "Assis": "Assistance", "Autom": "Automatic", "Auxil": "Auxiliary", "Batte": "Battery", "Beame": "Beamer", "Blowe": "Blower", "Boost": "Booster",
    "Borne": "Borne", "Brake": "Brake", "Camer": "Camera", "Catal": "Catalyst", "Ceili": "Ceiling", "Cell": "Cell", "Centr": "Central", "Chang": "Change",
    "Charg": "Charger", "Chass": "Chassis", "Clock": "Clock", "Clutc": "Clutch", "Colum": "Column", "Comfo": "Comfort", "Commu": "Communication",
    "Compr": "Compressor", "Condi": "Conditioning", "Contr": "Control", "Conve": "Converter", "Coola": "Coolant", "Corne": "Corner", "Cruis": "Cruise",
    "Dash": "Dash", "Board": "Board", "Deck": "Deck", "Lid": "Lid", "Devic": "Device", "Digit": "Digital", "Displ": "Display", "Door": "Door", "Drive": "Drive",
    "Dynam": "Dynamic", "Elect": "Electronics", "Energ": "Energy", "Engin": "Engine", "Exter": "External", "Fluid": "Fluid", "For": "for", "Front": "Front",
    "Fresh": "Fresh", "Funct": "Function", "Garag": "Garage", "Gatew": "Gateway", "Gear": "Gear", "Hands": "Hands", "On": "On", "Detec": "Detection",
    "Head": "Head", "Headl": "Headlight", "Heate": "Heater", "Heati": "Heating", "HV": "HV", "Infor": "Information", "Infot": "Infotainment",
    "Inter": "Interface", "Junct": "Junction", "Box": "Box", "Lane": "Lane", "Left": "Left", "Lift": "Lift", "Light": "Light", "Lock": "Lock", "Locki": "Locking",
    "Manag": "Management", "Matri": "Matrix", "Media": "Media", "Playe": "Player", "Posit": "Position", "Mobil": "Mobile", "Modul": "Module", "Monit": "Monitoring",
    "Motor": "Motor", "Mount": "Mount", "Multi": "Multi", "Night": "Night", "Visio": "Vision", "Noise": "Noise", "Opene": "Opener", "Parki": "Parking",
    "Parti": "Partition", "Passe": "Passenger", "Press": "Pressure", "Pump": "Pump", "Radar": "Radar", "Rear": "Rear", "Reduc": "Reduction", "Right": "Right",
    "Roll": "Roll", "Roof": "Roof", "Seat": "Seat", "Selec": "Selector", "Senso": "Sensor", "Shift": "Shift", "Side": "Side", "Softw": "Software", "Sound": "Sound",
    "Steer": "Steering", "Struc": "Structure", "Sub": "Sub", "Sunro": "Sunroof", "Syste": "System", "Telem": "Telematics", "Therm": "Thermal", "Tire": "Tyre",
    "Track": "Tracking", "Trail": "Trailer", "Trans": "Transmission", "Unit": "Unit", "Valve": "Valve", "Vehic": "Vehicle", "View": "View", "Wiper": "Wiper",
    "Axle": "Axle", "DCDC": "DC/DC", "Hand": "Hand", "Coola": "Coolant", "Pedal": "Pedal",
    "Assis2": "Assistance 2", "Inno": "Inno", "Interior": "Interior", "Exter": "External", "OTA": "OTA", "TV": "TV", "Tuner": "Tuner", "Up": "Up", "Clima": "Climate"}

VW_FIXED = {"DashBoard": "Instrument cluster", "BedienSG_vo_MSB": "Front control panel", "BedienSG_hi_MSB": "Rear control panel",
            "BedienSG_vo": "Front control panel", "BedienSG_hi": "Rear control panel"}

def vw_name(key, short=None):
    k = re.sub(r"^BV_", "", key)
    fixed = VW_FIXED.get(re.sub(r"(UDS|OBD)$", "", k))
    if fixed: return "%s (%s)" % (fixed, short) if short else fixed
    k = re.sub(r"(UDS|OBD)(_\w+)?$", "", k)
    if not re.fullmatch(r"[A-Za-z0-9]+", k) or len(k) < 4: return None
    toks = re.findall(r"[A-Z]{2,}(?=[A-Z][a-z]|\d|$)|[A-Z]?[a-z]+|\d+|[A-Z]+", k)
    out = []
    for t in toks:
        if t.isdigit(): out.append(t); continue
        w = VW_WORDS.get(t)
        if w is None and len(t) <= 4 and t.isupper(): w = t
        if w is None: return None
        out.append(w)
    name = re.sub(r"\bfor\b", "for", " ".join(out)).replace("Air Conditioning", "Air conditioning")
    return "%s (%s)" % (name, short) if short else name

def topology_links(proj):
    """logical link (without LL_) -> PIWIS short name, from Common/Topologie/*.xml of the newer lines"""
    out = {}
    for f in sorted(glob.glob(os.path.join(proj, "Common", "Topologie", "*.xml"))):
        x = open(f, encoding="utf-8", errors="replace").read()
        for nm, ll in re.findall(r'<ecu name="([^"]+)"[^>]*LogicalLink="LL_([^"]+)"', x): out.setdefault(ll, nm)
    return out

def old_name(key):
    k = re.sub(r"^_?(9x6|9x7|E1)_", "", key)
    for pat, nm in OLD_NAMES:
        if re.search(pat, k): return "%s (%s)" % (nm, k)
    return None

# VW TP 2.0 units behind the gateway: the databases do not carry their TP 2.0 address, so the standard VW
# diagnostic addresses are used. Marked "tpc": 0 in the pack (not confirmed on a car); a wrong address only
# means the channel does not open. Logical-link short name -> address.
TP20_ADDR = {"TUER_VL": 0x42, "TUER_VR": 0x52, "TUER_HL": 0x62, "TUER_HR": 0x72, "SITZ_FAHRER": 0x36, "SITZ_BEIFAHRER": 0x06,
             "ZUSATZHEIZUNG": 0x18, "ANHAENGER": 0x69, "RUECKFAHRKAMERA": 0x6C, "SWA": 0x3C}

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
                        r"Memory.?(Read|Write)|Transfer.?Data|Security.?Access|SecurAcces|Seed|Fingerprint|Kodiercontainer|"
                        r"protection.?of.?vehicle.?diagnosis|Schutz.?Fahrzeug.?Diagnose)")
# Engine calibration data (maps, characteristic values) is emissions-relevant: never offered as a write.
ENGINE_RE = re.compile(r"(?i)ECM|DME|BDE|MED\d|SIMOS|EDC\d|EnginContr|Motorsteuer|_bde$|sdi41|med91")
ENGINE_CAL_RE = re.compile(r"(?i)Calibrierdaten|Kalibrierdaten|Kalibrierung.?daten|calibration.?data|Kennfeld|Kennlinie|Applikationsdaten|application.?data")
EXCLUDE_SID = {0x23, 0x27, 0x28, 0x29, 0x34, 0x35, 0x36, 0x37, 0x3D, 0x83, 0x84, 0x85, 0x86, 0x87, 0x10, 0x11, 0x3E,
               0x14, 0x18, 0x19, 0x17, 0x12, 0x13}

SUFFIX_RE = re.compile(r"\s+(Read Data By Identifier|ReadDataByIdentifier|Write Data By Identifier|Request Routine Results|"
                       r"RequestRoutineResults?|Start Routine|StartRoutine|Stop Routine|StopRoutine|Short Term Adjustment|"
                       r"Return Control To E[Cc][Uu]|Freeze Current State|ReadDocumentation|WriteDocumentation|Read Read|Read|Write)$")

def svc_name(ln, sn=""):
    """service long name without the standard ODX service suffix (the category says what it is)"""
    n = (ln or "").strip()
    if not n or "<" in n or "\n" in n or len(n) > 140:        # a description, not a name: use the short name
        n = re.sub(r"^(DiagnServi|Diagnostic_Service)_", "", sn or "").replace("_", " ").strip()
    for _ in range(2): n = SUFFIX_RE.sub("", n)
    return n or (ln or sn or "")

# ---- environment data of a fault (UDS 19 06 FF) --------------------------------------------------------
# The standard records are the same on every control unit of a generation; their fields get fixed English
# names here so the app can show them uniformly (and merge the date fields into one time stamp).
ENV_NAMES = [(r"Haeufigkeit|OccurCount|Occurence|Occurrence|FrequencyCount", "Frequency counter"),
             (r"Verlern|Unlearn|Verlernzaehler", "Unlearning counter"), (r"AgingCount|Aging", "Aging counter"),
             (r"DTCPrior|Priority|Prioritaet", "Priority"), (r"Kilometerstand|KmMilea|Mileage|Odometer", "Mileage"),
             (r"^(Param_)?Year", "Year"), (r"^(Param_)?Month", "Month"), (r"^(Param_)?Day", "Day"), (r"^(Param_)?Hour", "Hour"),
             (r"^(Param_)?Minut", "Minute"), (r"^(Param_)?Secon", "Second"), (r"Voltage|Spannung", "Voltage")]
ENV_DROP = re.compile(r"(?i)reserv|Time_?Indication|^Bitfield$")

def env_params(pk, response):
    """Flat parameter list for the environment-data service: constants, then the fields of the standard
    records with absolute byte positions. Anything after them (unit-specific records) is left to the app,
    which shows it as raw bytes."""
    out = []; state = {"stop": False}
    def walk(d, base):
        for f in d.get("fields", []):
            if state["stop"]: return
            sn = f.get("sn", ""); dd = f.get("dop")
            m = re.fullmatch(r"ExtendedDataRecordNumber_?(\d+)", sn)
            if m and not (isinstance(dd, dict) and dd.get("type") in ("struct", "field")):
                out.append([-1, base + f["byte"], 0, 8, int(m.group(1)), -1]); continue
            if re.search(r"AllExtendedDataRecords|_Iter$|Iterator", sn): state["stop"] = True; return
            if "const" in f or ENV_DROP.search(sn) and not isinstance(dd, dict): continue
            if not isinstance(dd, dict): continue
            if dd.get("type") == "struct": walk(dd, base + f["byte"]); continue
            if dd.get("type") == "field": state["stop"] = True; return
            if dd.get("type") != "simple" or ENV_DROP.search(sn): continue
            name = next((pk.L(lbl) for pat, lbl in ENV_NAMES if re.search(pat, sn)), None)
            if name is None: name = pk.S(f.get("ln") or sn, sn)
            out.append([name, base + f["byte"], f["bit"], 0, None, pk.dop(dd)])
    for p in response or []:
        dd = p.get("dop")
        if "const" in p: out.append([-1, p["byte"], p["bit"], p.get("bits", 8), p["const"], -1]); continue
        if not isinstance(dd, dict) or p["byte"] < 6: continue          # DTC and status byte are shown already
        if dd.get("type") == "field": dd = dd.get("item") or {}
        if dd.get("type") == "struct": walk(dd, p["byte"])
        if state["stop"]: break
    return out if sum(1 for x in out if x[5] >= 0) >= 2 else None

def security_level(svcs):
    """[request-seed sub-function, seed bytes] of the coding level: the level named 'Codierung', else 'Login',
    else the lowest one defined."""
    cand = []
    for s in svcs:
        rq = s["request"]
        if len(rq) < 2 or rq[0].get("const") != 0x27 or not isinstance(rq[1].get("const"), int): continue
        lvl = rq[1]["const"]
        if lvl % 2 == 0 or any(isinstance(p.get("dop"), dict) for p in rq): continue        # send-key
        bits = [p["dop"].get("bits") for p in (s.get("response") or []) if isinstance(p.get("dop"), dict) and p["dop"].get("bits")]
        nm = s["sn"] + " " + s["ln"]
        rank = 0 if re.search(r"(?i)codier", nm) else 1 if re.search(r"(?i)login", nm) else 2 if not re.search(r"(?i)boot|download|upload|flash|system", nm) else 3
        cand.append((rank, lvl, (bits[0] // 8) if bits and bits[0] % 8 == 0 else 0))
    if not cand: return None
    cand.sort()
    return [cand[0][1], cand[0][2]] if cand[0][0] < 3 else None

def kline_link(e, name, links):
    """K-line parameters of a control unit that is not on the diagnostic CAN: [[address, tester]], [init, 5-baud, baud]"""
    own = e.comparams()
    best = None
    for ln, l in links.items():
        if not (ln.lstrip("_") == name.lstrip("_") or l.get("ecu") in e.ecu_ids): continue
        if "14230_2" in l.get("proto", "") and "CP_PhysReqTargetAddr" in l: best = l; break
    addr = (best or {}).get("CP_PhysReqTargetAddr")
    if addr is None:
        v = own.get("CP_PhysReqTargetAddr") or own.get("CP_EcuRespSourceAddress")
        addr = v[0] if v else None
        if addr is None or any(k.startswith("CP_Can") for k in own): return None
        best = {k: v[0] for k, v in own.items() if v}
    init = best.get("CP_InitializationSettings", 2)
    baud = best.get("CP_Baudrate", 10400)
    if not (1200 <= baud <= 115200): baud = 10400
    return [[addr & 0xFF, 0xF1]], [init if init in (1, 2) else 2, best.get("CP_5BaudAddressPhys", 0) & 0xFF, baud]

def can_link(e, name, links):
    for ln, l in links.items():
        if (ln.lstrip("_") == name.lstrip("_") or l.get("ecu") in e.ecu_ids) and "CP_CanPhysReqId" in l and "CP_CanRespUSDTId" in l:
            return [[l["CP_CanPhysReqId"], l["CP_CanRespUSDTId"]]]
    return None

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
    pk = Pack(lang); topo = topology_names(proj); links = ox.logical_links(proj); tlinks = topology_links(proj)
    groups = collections.OrderedDict()                 # CAN pair -> module
    excluded = [0]
    nocan = []
    for f in ox.ecu_files(proj):
        name = f[6:-6]
        e = ox.Ecu(proj, f)
        cod_algo = e.codierung_algo()
        ids = pair_ids(e.can_ids()); kl = None
        if not ids: ids = can_link(e, name, links) or []
        if not ids:
            k = kline_link(e, name, links)
            if k: ids, kl = k
        tplinks = [] if ids else [(n2, l) for n2, l in links.items() if "TP20" in l.get("proto", "") and
                                  (l.get("ecu") == ox.djb2(name) or l.get("ecu") in e.ecu_ids)]
        svcs = e.services(); dtcs = e.dtcs()
        proto = 1 if any(p.get("const") == 0x19 and p["byte"] == 0 for s in svcs for p in s["request"]) else \
                2 if any(p.get("const") == 0x18 and p["byte"] == 0 for s in svcs for p in s["request"]) else 0
        pr = (("K",) + tuple(ids[0]) if kl else tuple(ids[0])) if ids else None
        if pr and pr in groups: m = groups[pr]
        else:
            lk = next((l for n2, l in links.items() if n2.lstrip("_") == name.lstrip("_") or l.get("ecu") in e.ecu_ids), None)
            ln = topo.get(name, (name, (lk or {}).get("ln") or name))[1]
            nice = None
            if ln == name or ln.lstrip("_") == name.lstrip("_") or re.match(r"BV[ _]", ln):
                nice = old_name(name)
                if nice is None and name.startswith("BV_"):
                    base = re.sub(r"^BV_", "", name)
                    nice = vw_name(name, tlinks.get(base) or tlinks.get(re.sub(r"_\w+$", "", base)))
            m = {"k": name, "n": pk.L(nice) if nice else pk.S(ln, name), "ids": [list(p) for p in ids[:3]], "p": proto, "svc": [], "dtc": [],
                 "_seen": set(), "_dtcseen": set(), "dbs": [], "_src": [], "_cod": cod_algo}
            if kl: m["link"] = 1; m["kl"] = kl; m["p"] = 2
            if tplinks: m["_tp"] = tplinks
            if pr: groups[pr] = m
            else: nocan.append(m)
        m["dbs"].append(name)
        if "sa" not in m:
            sa = security_level(svcs)
            if sa: m["sa"] = sa
        if "_env" not in m:
            for s in svcs:
                rq = s["request"]
                if len(rq) > 1 and rq[0].get("const") == 0x19 and rq[1].get("const") == 0x06 and "DEFAULT" in (s.get("semantic") or ""):
                    m["_envsvc"] = True
                    ep = env_params(pk, s.get("response"))
                    if ep:
                        m["_env"] = [pk.L("Fault environment data"), "E", "ENVREAD", 1, [[-1, 0, 0, 8, 0x19, -1], [-1, 1, 0, 8, 0x06, -1]], ep]
                    break
        for s in svcs:
            if s["sn"] in m["_seen"]: continue
            m["_seen"].add(s["sn"])
            sid = s["request"][0].get("const") if s["request"] else None
            if sid == 0x29 or re.search(r"(?<![A-Za-z])SFD(?![a-z])|Schutz.?Fahrzeug.?Diagnose|protection.?of.?vehicle.?diagnosis", s["sn"] + " " + s["ln"], re.I):
                m["sfd"] = 1                               # unit protected by online authentication (see docs/SFD.md)
            if sid in EXCLUDE_SID or EXCLUDE_RE.search(s["sn"] + " " + s["ln"]) or \
               (sid in (0x2E, 0x3B) and ENGINE_RE.search(name) and ENGINE_CAL_RE.search(s["sn"] + " " + s["ln"])):
                excluded[0] += 1; continue
            mask = 0
            for x in s["sessions"]: mask |= SESS.get(x, 0)
            sec = 1 if any("Codier" in x or "CODIER" in x for x in s.get("security", [])) else 0
            m["_src"].append((s["sn"], s["ln"]))
            m["svc"].append([pk.S(svc_name(s["ln"], s["sn"]), s["sn"]), category(s.get("semantic"), s.get("class"), sid),
                             s.get("semantic", ""), (mask or 1) | (sec << 8), pk.params(s["request"]), pk.params(s.get("response"))])
        for disp, txt in sorted(dtcs.items()):
            bits, code = code_key(disp)
            if (bits, code) in m["_dtcseen"]: continue
            m["_dtcseen"].add((bits, code)); m["dtc"].append([code, bits, pk.S(txt)])
        progress("  %-34s %s svc %4d dtc %4d" % (name, (("K-line %02X" % ids[0][0]) if kl else ("TP2.0 (%d link(s))" % len(tplinks)) if tplinks else " ".join("%03X/%03X" % tuple(p) for p in ids[:2])) or "no CAN", len(svcs), len(dtcs)))
    # one module per TP 2.0 logical link (front doors share one database for left and right ...)
    tp_mods = []
    for m in list(nocan):
        tl = [(n2, l) for n2, l in m.pop("_tp", []) if n2 in TP20_ADDR]
        if not tl: continue
        nocan.remove(m)
        for n2, l in sorted(tl, key=lambda x: TP20_ADDR[x[0]]):
            t = dict(m); t["k"] = m["k"] if len(tl) == 1 else "%s_%s" % (m["k"], n2)
            t["n"] = pk.S(l.get("ln") or n2, n2); t["ids"] = [[TP20_ADDR[n2], 0]]; t["link"] = 2; t["p"] = 2; t["tpc"] = 0
            t["_base"] = m["k"]; t["_cod"] = m.get("_cod")
            tp_mods.append(t)
    for m in nocan: m.pop("_tp", None)
    mods = list(groups.values()) + tp_mods + nocan
    joblist = jobdefs.resolve(mods, pk, progress)
    # Environment data: the standard records (fault frequency, mileage, time stamp ...) are the same on all
    # control units of a line. Units whose own layout could not be resolved get the line's most common one;
    # the app checks the record numbers before it trusts the layout and falls back to raw bytes.
    sig = collections.Counter(json.dumps(m["_env"][5]) for m in mods if m.get("_env"))
    std = json.loads(sig.most_common(1)[0][0]) if sig else None
    nown = nstd = 0
    for m in mods:
        m.pop("_seen", None); m.pop("_dtcseen", None); m.pop("_src", None)
        env = m.pop("_env", None); had = m.pop("_envsvc", False)
        if env: nown += 1
        elif had and std:
            env = [pk.L("Fault environment data"), "E", "ENVREAD_STD", 1, [[-1, 0, 0, 8, 0x19, -1], [-1, 1, 0, 8, 0x06, -1]], std]; nstd += 1
        if env: m["svc"].append(env)
    progress("  fault environment data: %d units with their own layout, %d with the line's standard layout" % (nown, nstd))
    for m in mods:
        # coding-unlock algorithm name (reference only; constants live in the local key file).
        # Only emitted when at least one service of this module needs a Codierung key.
        algo = skmap.algo_for(m.pop("_base", m["k"]), m.pop("_cod", None))
        if algo and any(len(s) > 3 and (s[3] & 0x100) for s in m["svc"]):
            m["sk"] = algo
    progress("  units with online authentication (SFD): %d" % sum(1 for m in mods if m.get("sfd")))
    progress("  excluded functions (anti-theft, odometer, crash, HV safety, flashing, raw protocol): %d" % excluded[0])
    plat = platforms.BY_ID.get(platform, {})
    return {"v": 1, "platform": platform, "title": title, "lang": lang, "vin": plat.get("vin", []), "pn": plat.get("pn", []),
            "sfd": 1 if plat.get("sfd") else 0,
            "jobs": joblist, "str": pk.strs, "dops": pk.dops, "mods": mods}, pk.missing

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
