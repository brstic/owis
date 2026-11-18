#!/usr/bin/env python3
"""Build the vehicle data packs from YOUR OWN installation of the manufacturer's diagnostic software.

OpenDiag ships no vehicle data. This builds it, in English, from the diagnostic database of an installation
you own and are licensed to use. It works offline: the English texts come from the translation cache that
ships with OpenDiag (tools/i18n/), so no translation model is needed.

Usage:
    python3 build_data.py <Projects-V folder> [<MIG Projects-V folder>] [--out <dir>] [--lang de] [ID ...]

  <Projects-V folder>  the folder of your installation that holds the per-line sub-folders (G1, 992, J1 ...),
                       e.g. ...\\PIDT\\Projects-V . For the older lines (996/986, 997/987, Cayenne 955) pass the
                       MIG folder too, e.g. ...\\PIDT_MIG\\Projects-V .
  --out <dir>          where to write the packs (default: ./data, read by the app next to the exe).
  --lang de            keep the original texts instead of English (local use only, never share such a pack).
  ID ...               build only these lines (e.g. G1 992); default: every line found.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_pack, platforms, json

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    lang = "de" if "--lang" in sys.argv and "de" in sys.argv[sys.argv.index("--lang") + 1:sys.argv.index("--lang") + 2] else "en"
    out = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data")
    roots = [a for a in args if os.path.isdir(a)]
    only = {a for a in args if not os.path.isdir(a)}
    if not roots:
        print(__doc__); print("ERROR: give the path to your installation's Projects-V folder."); return 2
    os.makedirs(out, exist_ok=True); built = 0
    for p in platforms.PLATFORMS:
        if only and p["id"] not in only: continue
        proj = next((os.path.join(r, p["dir"]) for r in roots if os.path.isdir(os.path.join(r, p["dir"]))), None)
        if not proj or not any(f.endswith(".bv.db") for f in os.listdir(proj)):
            continue
        print("==", p["id"], p["title"], flush=True)
        pack, missing = build_pack.build(proj, p["id"], p["title"], lang)
        suffix = "_de" if lang == "de" else ""
        path = os.path.join(out, p["id"] + suffix + ".json")
        json.dump(pack, open(path, "w", encoding="utf-8"), ensure_ascii=False, separators=(",", ":"))
        print("   -> %s  (%d control units, %d functions, %.1f MB, %d texts not translated)" % (
            os.path.abspath(path), len(pack["mods"]), sum(len(m["svc"]) for m in pack["mods"]),
            os.path.getsize(path) / 1e6, len(missing)))
        built += 1
    print("\nBuilt %d vehicle line(s). Put the 'data' folder next to the OpenDiag program." % built)
    if not built: print("No vehicle lines found under", roots, "- is this the Projects-V folder?")
    return 0

if __name__ == "__main__":
    sys.exit(main())
