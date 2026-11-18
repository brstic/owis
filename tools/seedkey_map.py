#!/usr/bin/env python3
"""Map each control-unit (module key, as it appears in a data pack) to the short-name of the
Codierung seed/key algorithm that unlocks its coding writes.

Only the algorithm *name* is produced here - a reference, never a constant. The public data pack
carries this name in each module's "sk" field; the actual unlock constants live in a separate local
key file (see build_keyfile.py) that is generated from your own PIWIS install and is never committed.

A wrong key can trip a control unit's SecurityAccess attempt counter / lockout, so only
high-confidence associations are listed. A module with no confident mapping gets no "sk": the app
then reports that coding writes for it need a key that is not available, instead of sending a guess.

992 and newer control units (the "BV_*UDS" keys) authenticate with online SFD, not a local
algorithm; they are intentionally left unmapped.
"""

# module key (pack "k") -> seed/key algorithm short-name (must exist in seedkey_catalog.json).
# Prefer the coding ("_Codierung"/"_COD"/"_cod") variant; hardware-revision variants (EPB_VR7/VR8,
# ECM..., BDE_W06) are resolved at runtime by the key file, which carries every variant.
ALIAS = {
    # --- gateway / network ---
    "CAN_CAN_Gateway":          "ASAM_seedkey_Gateway",
    # --- chassis / brakes / suspension ---
    "PSM":                      "ASAM_seedkey_PSM",
    "ESP_Colorado":             "ASAM_SeedKey_ESP_PPPPP",
    "EPB":                      "ASAM_seedkey_EPB",
    "PASM":                     "ASAM_seedkey_PASM",
    "PASM_PADM":                "ASAM_seedkey_PASM",
    "PDCC":                     "ASAM_seedkey_PDCC",
    "PDC":                      "ASAM_seedkey_PDC",
    "RDK":                      "ASAM_seedkey_RDK",
    "RDK3":                     "ASAM_seedkey_RDK",
    "Lift_VA":                  "ASAM_seedkey_Lift_VA",
    "Allrad":                   "ASAM_seedkey_Allrad_Codierung",
    "Quersperre":               "ASAM_seedkey_HAL",          # rear transverse lock / rear-axle actuator
    "Hinterachslenkung":        "ASAM_seedkey_HAL",          # rear-axle steering
    # --- steering ---
    "Lenkhilfe":                "ASAM_seedkey_EPHS",         # electric power steering
    "Lenkstockschalter":        "ASAM_Seedkey_KLSM",         # steering-column switch module
    "Lenkstockschalter2":       "ASAM_seedkey_KLSM2",
    # --- powertrain ---
    "Getriebesteuerung_PDK":    "ASAM_seedkey_PDK",
    # --- body / comfort ---
    "Airbag":                   "ASAM_seedkey_Airbag_Codierung",
    "Kombiinstrument":          "kombi_9x1_codierung",
    "Kombiinstrument_MM":       "kombi_9x1_codierung",
    "Kompass":                  "ASAM_seedkey_Kompass_Codierung",
    "Zusatzheizung":            "ASAM_Standheizung",
    "Heckklappe":               "Seedkey_PLG_Cod",           # power liftgate
    "Sitzsteuergeraet":         "ASAM_seedkey_SITZ",
    "Verdeck_Synchronisation":  "ASAM_seedkey_VDK_SYNC",
    "BCM_vorne":                "SeedKey_BCM_V",
    "BCM_hinten":               "SeedKey_BCM_H",
    "BKE":                      "ASAM_seedkey_BKE",
    "BMS2":                     "ASAM_seedkey_BMS",
    "SWA":                      "ASAM_seed_SWA_KALGE",
    "SWA2":                     "ASAM_seed_SWA_KALGE",
    "SWSG":                     "SWSG_Single_Seedkey",
    "SWSG_mit_AFS_ALWR":        "SWSG_Single_Seedkey",
    "ACC3":                     "ASAM_seedkey_ACC3",
}


# These hand overrides win over what the database states, for the few units where a specific variant is
# wanted: EPB uses the base name (the key file carries every VR7/VR8 hardware revision), Allrad / PASM
# likewise. Everything else is now read straight from the database (ox.Ecu.codierung_algo), which is
# authoritative - it is the algorithm PIWIS itself calls - so no module is mapped on a guess.
# Engine control units are deliberately left without a coding key: calibration / emissions-relevant engine
# changes are out of scope for OWIS, so their writes are never unlocked (reading, live data, routines
# and fault memory are unaffected).
import re as _re
NO_UNLOCK = _re.compile(r"(?i)ECM|DME|BDE|MED\d|SIMOS|EDC\d|EnginContr|Motorsteuer|_bde$|sdi41|med91")

def _family(a):
    """algorithm family: the name without its hardware-variant / level suffix (EPB_VR8 -> epb, PASM_VW -> pasm)"""
    return _re.sub(r"(?i)_(vr\d+|vw|codierung|cod|w\d+|\d+)$", "", a or "").lower()

def algo_for(module_key, db_algo=None):
    """Seed/key algorithm short-name for a control unit.
    - the database is the authority (db_algo, what PIWIS calls); a "Dummy" there means the unit has no real
      coding key, so none is used;
    - a hand override is kept only where it names the same algorithm family as the database (a base name
      whose hardware variants the key file carries), or where the database states nothing;
    - engine control units never get a key (calibration is out of scope)."""
    if NO_UNLOCK.search(module_key): return None
    if db_algo and "dummy" in db_algo.lower(): return None
    hand = ALIAS.get(module_key)
    if db_algo and hand and _family(hand) != _family(db_algo): return db_algo
    return hand or db_algo


if __name__ == "__main__":
    import json, os, glob, collections, sys
    here = os.path.dirname(os.path.abspath(__file__))
    cat_path = os.path.expanduser("~/Projects/porsche/seedkey/seedkey_catalog.json")
    cat = json.load(open(cat_path)) if os.path.exists(cat_path) else {}
    # sanity: every alias target must exist in the catalog
    bad = sorted({v for v in ALIAS.values() if cat and v not in cat})
    if bad:
        print("WARNING: alias targets not in catalog:", bad, file=sys.stderr)
    # coverage over the built packs
    data = os.path.join(here, "..", "data")
    seen = collections.Counter(); mapped = collections.Counter()
    for f in glob.glob(os.path.join(data, "*_de.json")):
        d = json.load(open(f))
        for m in d["mods"]:
            seen[m["k"]] += 1
            if algo_for(m["k"]): mapped[m["k"]] += 1
    tot = len(seen); mp = len(mapped)
    print("module keys seen: %d   mapped: %d" % (tot, mp))
    print("mapped:", ", ".join(sorted(mapped)))
