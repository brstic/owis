#!/usr/bin/env python3
"""Build all vehicle data packs from a local PIWIS installation.

For each vehicle line: German pack (private, data/<ID>_de.json) -> machine translation of the new texts
(tools/translate_mt.py, needs the MT venv) -> English pack (data/<ID>.json, publishable).

Usage: python3 -I make_packs.py <Projects-V dir> <MT python> <MT model dir> [ID ...] [--de-only]
       (<MT python> and <MT model dir> may be "-" with --de-only)
"""
import os, sys, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from platforms import PLATFORMS

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    de_only = "--de-only" in sys.argv
    root, mtpy, model = args[0], args[1], args[2]
    only = set(args[3:])
    here = os.path.dirname(os.path.abspath(__file__)); data = os.path.join(here, "..", "data")
    os.makedirs(data, exist_ok=True)
    for p in PLATFORMS:
        pid, title = p["id"], p["title"]
        if only and pid not in only: continue
        proj = os.path.join(root, p["dir"])
        if not os.path.isdir(proj) or not any(f.endswith(".bv.db") for f in os.listdir(proj)):
            print("skip", pid, "(not available)"); continue
        de = os.path.join(data, pid + "_de.json"); en = os.path.join(data, pid + ".json")
        print("==", pid, title, flush=True)
        subprocess.run([sys.executable, "-I", os.path.join(here, "build_pack.py"), proj, pid, de, "--lang", "de", "--title", title], check=True)
        if de_only: continue
        subprocess.run([mtpy, os.path.join(here, "translate_mt.py"), model, de], check=True)
        subprocess.run([sys.executable, "-I", os.path.join(here, "build_pack.py"), proj, pid, en, "--title", title], check=True)

if __name__ == "__main__":
    main()
