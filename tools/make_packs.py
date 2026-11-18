#!/usr/bin/env python3
"""Build all vehicle data packs from a local PIWIS installation.

For each vehicle line: German pack (private, data/<ID>_de.json) -> machine translation of the new texts
(tools/translate_mt.py, needs the MT venv) -> English pack (data/<ID>.json, publishable).

Usage: python3 -I make_packs.py <Projects-V dir> <MT python> <MT model dir> [ID ...]
"""
import os, sys, subprocess, json

PLATFORMS = [
    ("G1",        "Panamera (970) 2010-2016"),
    ("G2",        "Panamera (971) 2017-2024"),
    ("9x1",       "911 (991) / Boxster, Cayman (981, 718) 2012-2019"),
    ("CS1",       "911 / Cayman Clubsport race cars (CS1)"),
    ("918S",      "918 Spyder 2013-2015"),
    ("992",       "911 (992) 2019-"),
    ("Cajun",     "Macan (95B) 2014-"),
    ("E2",        "Cayenne (92A) 2011-2017"),
    ("E3",        "Cayenne (9Y0) 2018-"),
    ("J1",        "Taycan (9J1) 2020-"),
    ("MIG/9x7",   "911 (997) / Boxster, Cayman (987) 2005-2012"),
    ("MIG/9x6",   "911 (996) / Boxster (986) 1997-2005"),
    ("MIG/E1",    "Cayenne (9PA) 2003-2010"),
]

def main():
    root, mtpy, model = sys.argv[1], sys.argv[2], sys.argv[3]
    only = set(sys.argv[4:])
    here = os.path.dirname(os.path.abspath(__file__)); data = os.path.join(here, "..", "data")
    os.makedirs(data, exist_ok=True)
    for pid, title in PLATFORMS:
        if only and pid not in only: continue
        proj = os.path.join(root, pid)
        if not os.path.isdir(proj) or not any(f.endswith(".bv.db") for f in os.listdir(proj)):
            print("skip", pid, "(not available)"); continue
        tag = pid.replace("MIG/", "")
        de = os.path.join(data, tag + "_de.json"); en = os.path.join(data, tag + ".json")
        print("==", pid, title, flush=True)
        subprocess.run([sys.executable, "-I", os.path.join(here, "build_pack.py"), proj, tag, de, "--lang", "de", "--title", title], check=True)
        subprocess.run([mtpy, os.path.join(here, "translate_mt.py"), model, de], check=True)
        subprocess.run([sys.executable, "-I", os.path.join(here, "build_pack.py"), proj, tag, en, "--title", title], check=True)

if __name__ == "__main__":
    main()
