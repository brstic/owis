# PorscheTool — development notes

Open-source, PIWIS-independent diagnostics for Porsche vehicles over a cheap J2534 cable. This file records what
has been built and what remains, so work can resume cleanly.

## Goal

A complete, free replacement for the dealer tester (PIWIS) for the diagnostic jobs an independent workshop and an
owner need: read/clear faults on every control unit, live data, identification, actuator tests, routines / basic
settings and adaptation, in English, driven by data extracted from a local PIWIS installation. Right-to-repair.

Explicitly **out of scope** (never implemented): odometer writes, immobiliser / key learning / component protection,
airbag crash-record manipulation, high-voltage battery safety disable, emissions-relevant engine changes.

## Architecture

```
PIWIS install ──tools/odxdb.py──> per-ECU services (requests, decoded responses, DTCs, CAN ids)
             └─tools/build_pack.py─> data/<LINE>.json   (one pack per vehicle line)
                                      ↑ English texts: tools/i18n/*.json (hand) + mt (machine)
src/pack.h (+ json.h)  loads a pack, builds requests, decodes responses (ODX semantics)
src/porschetool.cpp    Win32 app: J2534 transport (UDS+KWP, 11/29-bit), scan/clear, module window
```

The Windows app ships only `PorscheTool.exe` + the `data/*.json` packs. No PIWIS, no Java, no database at runtime.

### Data pack format (`data/<LINE>.json`, version 1)

Compact JSON: `{v, platform, title, lang, str[], dops[], mods[]}`. All text is referenced by index into `str`.
- `dops[]`: a decoded ODX data-object — `["s", base, bits, lentype, msb, compu, scales, unit, mask]` (simple value),
  `["t", fields]` (structure), `["f", item]` (repeated field).
- `mods[]`: `{k: key, n: name, ids: [[req,resp]...], p: proto, svc: [...], dtc: [[code,bits,text]...]}`.
- `svc`: `[name, category, semantic, sess_and_flags, request_params, response_params]`.
  category: **L** live data · **I** identification · **A** actuator test · **R** routine/basic setting ·
  **C** adaptation/coding read · **W** adaptation/coding write · **O** other.
  `sess_and_flags`: low byte = session bitmask; **bit 8 = needs Codierung (seed/key) unlock**.
- `param`: `[name, byte, bit, bits, const|null, dop|-1]`.

German packs (`<LINE>_de.json`) keep the original texts for local use; the English packs are the publishable ones.
English texts are keyed by `djb2(source)` so a pack carries no original PIWIS text — same redaction rule as
[pdk-3000-tool].

## The ODX database decoder (`tools/odxdb.py`)

The compiled PIWIS databases are `0.0.0@<ECU>.bv.db` = concatenated zlib streams, one ODX object each; record tag in
byte 0 (0xBE service, 0x78 request, 0x91 positive response, 0xAA structure, 0x2C simple DOP, 0xAC table row,
0xAB table, 0x57 DTC, 0x31 directory, 0x02 unit). Strings are referenced by `djb2(text) & 0x7FFFFFFF`, resolved via
`A/UStringData`. Objects are found by short-name hash through the 0x31 directories and the `.key` B-tree
(leaf pages only, validated against the directory). Shared layers (`FunctionalGroupUDS.fg`, `…KWP2000.fg`,
`ES_Data_Lib.sd`) are loaded too, for cross-referenced DOPs/units.

Key format facts worked out:
- PARAM sub-record: `01 <ptype> 00 | LONG | SHORT | … | bitpos | bytepos u32 | kind`; `27` CODED-CONST,
  `b2` table key, `b0` table struct.
- Simple DOP coefficients: `01 14 00 <n:u8> <n doubles> [01 <den double>]`; 2nd block = internal→physical.
- **Two table layouts**: ODX 2.0 (G1/9x1/918): row carries a numeric key. ODX 2.2 (992/newer): row key is a *text*
  resolved through the table's own key-DOP text table. `table_row()` handles both.
- Units: tag 0x02 record, display name at +26 (°C, %, mm, km/h …).

Coverage check: `scratchpad` cov script — on G1, 13k services, >98% of DOPs resolved; on 992, response structs
resolved ~97% (remainder are library key-DOPs / genuinely empty structs).

## Translation (`tools/translate_mt.py`)

Offline machine translation of the German texts: Argos/OpenNMT `de→en` model run through CTranslate2 +
SentencePiece, with an automotive glossary applied before and after. Results cached by `djb2(source)` in
`tools/i18n/mt_en.json`. Hand translations in other `tools/i18n/*.json` always win. ODX service-name suffixes
("… Read Data By Identifier", "… Start Routine") are stripped; the category already conveys the kind.
Setup: a venv with `ctranslate2 sentencepiece` + the unzipped `translate-de_en-1_3.argosmodel`.

`tools/make_packs.py` runs the whole chain (German pack → translate new texts → English pack) for every line.

## The app (`src/porschetool.cpp`)

- **J2534** via the installed driver (registry `PassThruSupport.04.04`, 32-bit view; falls back to `op20pt32.dll`).
  32-bit build so it can load the Tactrix 32-bit DLL. `__stdcall` typedefs. 11-bit and 29-bit CAN.
- **Scan all / clear all**: every module, UDS `19 02` / KWP `18 00`, automatic protocol fallback; identification via
  `22 F187/F189/F191/F197` or `1A 9B`. Report saved next to the exe. Session log always on.
- **Module window**: functions grouped by category with a search box. RUN / LIVE (repeat) / STOP, read & clear faults.
  - live data: repeats the read, shows the decoded values with units, tester-present kept alive.
  - routines: start, then poll the result service once a second and show status text until it ends or STOP; stop
    routine sent on exit.
  - actuator tests: run, hold with tester-present until STOP, then return-control-to-ECU.
  - adaptation write (2E): reads and logs the current value first, confirms, writes, reads back.
  - inputs dialog: texttable values become a dropdown (e.g. ride-height level), numbers an edit box.
- **CLI**: `--scan`, `--clear-all`, `--vehicle <ID>`, `--run <module> "<function>" [inputs...]` for scripted use.

### Tested without a car

`tests/test_core.cpp` (fault parsing/formatting) and `tests/test_pack.cpp` (request build + response decode against a
real pack, plus a sweep over every service). `tests/mock_j2534.cpp` is a fake `op20pt32.dll` that simulates a PDK, a
PASM (ride-height read 500 mm, levelling routine with a session check), an airbag unit, a gear selector that refuses
UDS and an engine unit that answers "response pending". Run under Wine: scan, clear-all and `--run` all verified.

## Vehicle coverage (data packs)

Built & verified: **G1** (Panamera 970), **9x1** (911 991 / 981 / 718), **CS1**, **918S**, **992** (911 992,
ODX 2.2), **Cajun** (Macan), **E2** (Cayenne 92A), **E3** (Cayenne 9Y0). Next: **J1** (Taycan),
**MIG/9x7/9x6/E1** (997/987, 996/986, Cayenne 955). Each pack ≈ 8k–22k services across 15–96 control units.
KWP lines (G1/9x1/918) mix UDS+KWP; 992/newer are UDS with 29-bit addressing on many modules.

## Seed/key (coding unlock) — see §14 of PDK_TCU_KNOWLEDGE.md

Needed only for coding **writes** (0x2E) and a few actuator tests; reading, routines and most tests need no key.
87 module jars catalogued (local, `~/Projects/porsche/seedkey/`). The former 21 "array-hash" jars were
reverse-engineered from the jar bytecode (javap) and fall into seven algorithm families, each **verified
against the jar's own seed→key vectors** before shipping (a wrong port could trip a control unit's attempt
counter, so nothing is emitted that does not match 100%):

| family | key formula | jars |
|--------|-------------|------|
| `kmul`  | `((seed^K)*A+B) mod 2^32` (incl. `~seed` as K=0xFFFFFFFF, and the kKrypto jars) | 61 |
| `table` | 2-byte rotate table lookup | 7 |
| `bmrot` | rotate-left/right by bit-selected count, then AND/OR/XOR/NOT | ACC3, BDE_COD(+W06), HR3 |
| `dia`   | 24/32-bit rotate-LFSR bit-hash over seed‖fixed, then nibble packing | EPB, EPB_VR7, EPB_VR8 |
| `lfsr`  | shift-left + conditional XOR const, 9 rounds | ESP_DL, EPB_dl |
| `kombi` | byte permutation + 31 add/XOR rounds | kombi_9x1_codierung |
| `rsr`   | add const, rotate-right ×2, conditional XOR const, 26 rounds | Airbag_Codierung |

79 algorithms ship in the key file; all **36** modules mapped by `seedkey_map.py` unlock. 992+ uses online
SFD, not computable offline. Still unshipped (out of scope / not coding): airbag memory download & upload,
BDE download/programming. The RE harness is `~/Projects/porsche/seedkey/` (Params.java dumps fields +
vectors, extract_array.py ports & verifies each family). Full write-up in PDK_TCU_KNOWLEDGE.md §14.

**Wired into the tool (verified, no car):**
- `tools/seedkey_map.py` — maps a module key (pack `k`) to its coding seed/key algorithm *name* only. 36
  high-confidence modules mapped; a wrong key can trip a control unit's SecurityAccess attempt counter, so
  unmapped modules get no key and the app says so rather than guessing. `BV_*UDS` (992+) are left unmapped (SFD).
- `build_pack.py` now emits `"sk": <algo-name>` on each module that has a Codierung-flagged service (bit 8).
- `tools/build_keyfile.py` — turns the local catalogue (+ `full_tables/`) into a single binary
  `porschetool.keys` (magic `PTKEY1`): 57 keyed-multiply + 7 rotate-table algorithms. **Local only, gitignored**
  — it holds the unlock constants. Array-hash algorithms are not in it yet.
- `src/seedkey.h` — loads `porschetool.keys`, computes the key (`((seed^K)*A+B) mod 2^8nb`, or a table lookup).
  Verified byte-for-byte against the Python reference for keyed-multiply, and against the dumped tables.
- `src/porschetool.cpp` — `securityUnlock()` runs 0x27 (request seed at the coding level, else the generic
  level; compute; send key) and is called automatically before a bit-8 coding write and on an NRC 0x33 retry.
  All 36 mapped modules unlock. Without the key file the app still does everything except coding writes.
  The seed/key maths (all seven families) is verified byte-for-byte against the PIWIS jar vectors in `seedkey.h`
  (native `skvec` harness, 960 vectors); the on-car 0x27 request/response exchange itself is pending a
  mock/real-car confirmation (as with the earlier PdkTool).

## TODO (next sessions)

1. Build packs for the remaining lines (Cajun, E2, E3, J1, 9x7/9x6/E1).
2. Confirm the 0x27 seed/key exchange against the mock / a real car (the key maths is already verified); extend
   the module→algorithm map beyond the 36 high-confidence entries as more are confirmed.
3. Publish `porsche-tool` as a public repo (banner, README, release) — same redaction rules as pdk-3000-tool.
4. Freeze-frame / environment data decoding for UDS DTCs (SDG files hold the system reaction per fault).
5. Per-module safety notes for the risky write/actuator functions.
