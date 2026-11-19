# Seed/key (Codierung) extraction

Diagnostic coding seed/key needed to write coding/calibration after a repair (right-to-repair). Component-protection
and immobiliser keys are out of scope.

These scripts extract the algorithm from a local PIWIS installation's `ASAM_seedkey_*.jar` files; the resulting
per-module key constants are **not** stored in this repo (they are the unlock keys). Run against your own PIWIS.

- `Dump.java` / `DumpFull.java` — load a jar by reflection (stub `common.java.ASAM_seedkey` interface) and print
  (seed → key) vectors.
- `Params.java` — dump each jar's private constant fields **and** sample (seed → key) vectors, for the jars whose
  algorithm is reverse-engineered from bytecode.
- `derive.py` — fit the simple families (affine / xor / 2-byte table) from vectors.
- `km.py` — extract and verify the keyed-multiply constants `key = ((seed ^ K) * A + B) & 0xFFFFFFFF`.
- `extract_array.py` — port the reverse-engineered families (bmrot, dia, lfsr, kombi, rsr, kKrypto→kmul,
  ~seed→kmul) from the `Params.java` dump and **verify each against the jar's vectors** before emitting
  `array_ported.json` + `array_vectors.json`.

Seven algorithm families cover every mapped module; `../build_keyfile.py` turns the catalogue + ports into the
binary `owis.keys`. Nothing is emitted that does not match the jar's own vectors 100% (a wrong key can trip
a control unit's attempt counter).
