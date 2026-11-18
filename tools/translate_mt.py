#!/usr/bin/env python3
"""Machine-translate the untranslated texts of German data packs into English, offline.

Uses an OpenNMT de->en model (Argos Translate package, CTranslate2 + SentencePiece) plus an automotive
glossary applied before and after translation. Results are cached in tools/i18n/mt_en.json, keyed by the
djb2 hash of the source text (so no source texts are stored). Hand translations (other files in
tools/i18n/) always win over machine translations.

Setup:  python3 -m venv .venv && .venv/bin/pip install ctranslate2 sentencepiece
        download https://argos-net.com/v1/translate-de_en-1_3.argosmodel and unzip it
Usage:  .venv/bin/python translate_mt.py <model dir> data/G1_de.json [data/992_de.json ...]
"""
import sys, os, re, json, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from odxdb import djb2

# German term -> English term, applied to the source before translation (the model copies English tokens)
PRE = [
    (r"\bSG[- ]?Tausch\b", "control unit replacement"), (r"\bSteuerger(ä|ae)te?(s|n)?\b", "control unit"),
    (r"\bSG\b", "control unit"),
    (r"Kurzschluss\s+(nach|gegen)\s+Masse", "short to ground"), (r"Kurzschluss\s+(nach|gegen)\s+(Plus|UBat|Ubatt|Batterie)", "short to battery"),
    (r"Schluss\s+nach\s+Masse", "short to ground"), (r"\bnach\s+Masse\b", "to ground"), (r"\bMasseschluss\b", "short to ground"),
    (r"\bUnterbrechung\b", "open circuit"), (r"\bunplausibel\b", "implausible"), (r"\bUnplausibl\w*", "implausible"),
    (r"\bLuftfeder(n|ung)?\b", "air spring"), (r"\bD(ä|ae)mpfer\b", "damper"),
    (r"\bEntl(ü|ue)ftung(s|en)?\b", "bleeding"), (r"\bentl(ü|ue)ften\b", "bleed"),
    (r"\bVerlernz(ä|ae)hler\b", "unlearn counter"), (r"\bRennstart\w*", "launch control"),
    (r"\bausregeln\b", "adjust"), (r"\bFahrgestellnummer\b", "VIN"), (r"\bH(ä|ae)ndlernummer\b", "dealer number"),
    (r"\bWegsensor(wert)?\b", "travel sensor"), (r"\bStellgliedtest\b", "actuator test"), (r"\bStellglied(er)?\b", "actuator"),
    (r"\bGrundeinstellung\b", "basic setting"), (r"\b(An|Ein)lernen\b", "teach-in"), (r"\bLernwerte?\b", "learned values"),
    (r"\b(Codierung|Kodierung)\b", "coding"), (r"\bBotschaft\b", "message"), (r"\bFehlerspeicher\b", "fault memory"),
    (r"\bIstwert\b", "actual value"), (r"\bSollwert\b", "target value"), (r"\bErsatzma(ß|ss)nahmen\b", "substitute reactions"),
    (r"\b(zur(ü|ue)cksetzen|r(ü|ue)cksetzen)\b", "reset"), (r"\bSchaltstange(n)?\b", "shift rod"),
    (r"\bAbgleich\b", "calibration"), (r"\bBef(ü|ue)llung\b", "filling"), (r"\bNiveau\b", "level"),
    (r"\bKl(emme|\.)\s*(15|30|31|50|58|X)\b", r"terminal \2"), (r"\bZ(ü|ue)ndung\b", "ignition"),
    (r"\bFeststellbremse\b", "parking brake"), (r"\bBremsbel(ä|ae)ge?\b", "brake pads"), (r"\bDrehzahlf(ü|ue)hler\b", "wheel speed sensor"),
    (r"\bLenkwinkel\b", "steering angle"), (r"\bGurtstraffer\b", "belt pretensioner"), (r"\bSitzbelegterkennung\b", "seat occupancy detection"),
    (r"\bTiefniveau\b", "low level"), (r"\bHochniveau\b", "high level"), (r"\bNormalniveau\b", "normal level"),
    (r"\bKupplung\b", "clutch"), (r"\bGetriebe(ö|oe)l\b", "transmission oil"),
    (r",?\s*\bKDI\b", " (workshop)"), (r",?\s*\bBDI\b", " (factory)"),
    (r"\bRollenmodus\b", "dyno mode"), (r"\bRollenpr(ü|ue)fstand\w*", "dyno"), (r"\bBandende\w*", "end-of-line"),
    (r"\bH(ö|oe)hensensor(ik|en)?\b", "ride height sensor"), (r"\bEinschleif\w*", "bedding-in"),
    (r"\bLuft(spalt|spiel)\b", "air gap"), (r"\bKilometerstand\b", "mileage"), (r"\bBetriebsdauer\b", "operating time"),
    (r"\bTransportmodus\b", "transport mode"), (r"\bFahrzeugvariante\b", "vehicle variant"), (r"\bSpannung\b", "voltage"),
    (r"\bZeitstempel\b", "timestamp"), (r"\bAktuator\w*", "actuator"), (r"\bKundendienst\w*", "customer service"),
    (r"\bDruckpr(ü|ue)fung\b", "pressure test"), (r"\bP(ü|ue)fung(s)?\w*", "test"), (r"\bKurztest\b", "short test"),
    (r"\bTrimmregelung\b", "trim control"), (r"\bRennstartz(ä|ae)hler\b", "launch control counter"),
    (r"\bFederbein(e)?\b", "strut"), (r"\bbef(ü|ue)llen\b", "fill"), (r"\bHaeufigkeitsz(ae|ä)hler|H(ä|ae)ufigkeitsz(ä|ae)hler\b", "frequency counter"),
    (r"\bWegsensorik\w*", "travel sensor"), (r"\bSchaltstangen\b", "shift rods"), (r"\bKupplungen\b", "clutches"),
    (r"\bGetriebe(steuerung)?\b", "transmission"), (r"\bDrucksteller\b", "pressure actuator"), (r"\bAnsteuerung\b", "activation"),
]
POST = [
    (r"\bshort circuit (to |against )?(mass|ground|earth)\b", "short to ground"), (r"\bmass\b", "ground"),
    (r"\bventilation\b", "bleeding"), (r"\bForgetting [Cc]ounters?\b", "unlearn counter"),
    (r"\bstarting race counter\b", "launch control counter"), (r"\bchassis number\b", "VIN"),
    (r"\b[Rr]econciliation\b", "calibration"), (r"\brestocking\b", "filling"), (r"\bcontrol device\b", "control unit"),
    (r"\bUnlearned counter\b", "unlearn counter"), (r"\bCrush test\b", "pressure test"),
    (r"\s+", " "),
]
EN_WORDS = None

def is_english(t):
    global EN_WORDS
    if EN_WORDS is None:
        EN_WORDS = set()
        if os.path.exists("/usr/share/dict/words"): EN_WORDS = set(w.strip().lower() for w in open("/usr/share/dict/words"))
        EN_WORDS |= {"can", "lin", "ecu", "dtc", "obd", "vin", "id", "sw", "hw", "nr", "no", "max", "min", "temp", "info",
                     "config", "calibration", "init", "reset", "status", "data", "read", "write", "routine", "results", "identifier"}
    if re.search("[äöüÄÖÜß]", t): return False
    words = re.findall(r"[A-Za-z]{3,}", re.sub(r"([a-z])([A-Z])", r"\1 \2", t.replace("_", " ")))
    if not words: return True
    hit = sum(1 for w in words if w.lower() in EN_WORDS or w.lower().rstrip("s") in EN_WORDS)
    return hit / len(words) >= 0.75

def prep(t):
    s = t.replace("_", " ")
    s = re.sub(r"([a-zäöü])([A-Z])", r"\1 \2", s) if " " not in s.strip() and "_" in t else s
    for a, b in PRE: s = re.sub(a, b, s)
    return s

def post(t):
    for a, b in POST: t = re.sub(a, b, t)
    t = t.strip()
    return t[:1].upper() + t[1:] if t[:1].islower() else t

def pretty(t):
    """English technical names arrive as identifiers: Rear_left_ABS_inlet_valve -> Rear left ABS inlet valve"""
    if "_" in t and " " not in t.strip(): t = re.sub(r"_+", " ", t).strip()
    return t

def main():
    refresh = "--refresh" in sys.argv          # re-translate cached texts the glossary has rules for
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    model = args[0]; packs = args[1:]
    import ctranslate2, sentencepiece as spm
    dev = "cpu"
    tr = ctranslate2.Translator(os.path.join(model, "model"), device=dev, inter_threads=4, intra_threads=2)
    sp = spm.SentencePieceProcessor(model_file=os.path.join(model, "sentencepiece.model"))
    known = {}
    for f in sorted(os.listdir(os.path.join(HERE, "i18n"))):
        if f.endswith(".json") and not f.startswith("untranslated") and not f.startswith("mt_"):     # hand translations only
            known.update(json.load(open(os.path.join(HERE, "i18n", f), encoding="utf-8")))
    cache_p = os.path.join(HERE, "i18n", "mt_en.json")
    cache = json.load(open(cache_p, encoding="utf-8")) if os.path.exists(cache_p) else {}
    todo = {}
    for p in packs:
        d = json.load(open(p, encoding="utf-8"))
        units = set(x[7] for x in d["dops"] if x[0] == "s" and x[7] >= 0)
        for i, t in enumerate(d["str"]):
            if i in units or not t.strip(): continue
            h = "%08x" % djb2(t.strip())
            if h in known: continue
            if h in cache and not (refresh and any(re.search(a, t) for a, b in PRE)): continue
            if is_english(t): cache[h] = pretty(t.strip()); continue
            todo[h] = t.strip()
    print("to translate:", len(todo), flush=True)
    items = list(todo.items()); t0 = time.time()
    for k in range(0, len(items), 512):
        chunk = items[k:k + 512]
        src = [prep(t) for h, t in chunk]
        toks = [sp.encode(s, out_type=str)[:200] for s in src]
        res = tr.translate_batch(toks, beam_size=2, max_batch_size=64, max_decoding_length=200)
        for (h, t), r in zip(chunk, res):
            cache[h] = post(sp.decode(r.hypotheses[0]).replace("▁", " "))
        if (k // 512) % 10 == 0:
            json.dump(cache, open(cache_p, "w", encoding="utf-8"), ensure_ascii=False, indent=0, sort_keys=True)
            print("  %d / %d  (%.0f/s)" % (k + len(chunk), len(items), (k + len(chunk)) / max(1, time.time() - t0)), flush=True)
    json.dump(cache, open(cache_p, "w", encoding="utf-8"), ensure_ascii=False, indent=0, sort_keys=True)
    print("done, cache size", len(cache))

if __name__ == "__main__":
    main()
