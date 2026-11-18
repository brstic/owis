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
PIWIS install ──tools/odxdb.py──> per-ECU services (requests, decoded responses, DTCs, CAN ids, K-line links)
             └─tools/build_pack.py─> data/<LINE>.json   (one pack per vehicle line, tools/platforms.py)
                                      ↑ English texts: tools/i18n/*.json (hand) + mt (machine)
                                      ↑ guided jobs: tools/jobs.py (rules, resolved per line at build time)
src/pack.h (+ json.h)  loads a pack, builds requests, decodes responses (ODX semantics)
src/engine.h           everything done with a car: scan/clear, environment + snapshot data, functions,
                       coding unlock, jobs, VIN detection, reports            (portable, no Windows)
src/bus.h              transport interface   ─ j2534_bus.h (Windows: CAN ISO-TP + K-line ISO 14230)
                                             ─ socketcan_bus.h (Linux: kernel ISO-TP)
                                             ─ sim.h (simulated car)   ─ trace.h (record / replay)
src/porschetool.cpp    Win32 app (main window, module window, jobs window, live graph)
src/cli.cpp            porschetool-cli for Linux / macOS / scripts
src/safety.h           plain-language hazard notes shown before anything that changes or moves something
```

The Windows app ships only `PorscheTool.exe` + the `data/*.json` packs. No PIWIS, no Java, no database at runtime.

### Data pack format (`data/<LINE>.json`, version 1)

Compact JSON: `{v, platform, title, lang, str[], dops[], mods[]}`. All text is referenced by index into `str`.
- `dops[]`: a decoded ODX data-object — `["s", base, bits, lentype, msb, compu, scales, unit, mask]` (simple value),
  `["t", fields]` (structure), `["f", item]` (repeated field).
- top level also: `vin` (VIN model types → auto-detection), `pn` (part-number prefixes), `sfd` (line uses online
  authentication for coding), `jobs` (guided jobs: `{id, t, d, w, steps: [[mod, svc, ask, inputs, label]]}`, an input
  is a number, `null` = asked, or `"$y" "$M" "$D" "$ymd"` = today's date).
- `mods[]`: `{k: key, n: name, ids: [[req,resp]...], p: proto, svc: [...], dtc: [[code,bits,text]...]}`; optional
  `sk` (coding algorithm name), `sa` `[seed sub-function, seed bytes]` of the coding level, `link: 1` + `kl: [init,
  5-baud address, baud]` for K-line units (then `ids = [[ECU address, tester address]]`).
- `svc`: `[name, category, semantic, sess_and_flags, request_params, response_params]`.
  category: **L** live data · **I** identification · **A** actuator test · **R** routine/basic setting ·
  **C** adaptation/coding read · **W** adaptation/coding write · **O** other · **E** layout of the fault environment
  data (UDS `19 06`), used by the fault reader and not listed as a function.
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

## The engine and the apps

- **Transports**: J2534 (registry `PassThruSupport.04.04`, 32-bit view; falls back to `op20pt32.dll`; `__stdcall`;
  ISO 15765 on 11/29-bit CAN, ISO 14230 on the K-line with fast or 5-baud init), Linux SocketCAN (kernel ISO-TP),
  the simulated car, and replay of a recorded session.
- **Scan all / clear all**: every module, UDS `19 02` / KWP `18 00`, automatic protocol fallback; identification via
  `22 F187/F189/F191/F197` or `1A 9B`. For every UDS fault: environment data `19 06 <DTC> FF` decoded with the
  unit's own layout (or the line's standard records), snapshot `19 04 <DTC> FF` decoded through the unit's read
  functions for each data identifier, full status-byte breakdown. Text + HTML report next to the exe.
- **Vehicle detection**: VIN (`22 F190`, KWP `1A 90`, OBD `09 02`) → model type (VIN positions 7/8/12, US VINs
  handled) → pack; without a VIN, the part numbers of the answering units vote for a line. Runs by itself the first
  time the adapter is found.
- **Module window**: functions grouped by category with a search box. RUN / LIVE (several values at once) / GRAPH /
  STOP, read & clear faults. Routines are followed until their status says finished or failed; actuator tests are
  held with tester-present until STOP; writes read and log the current value first and read back after.
  Inputs: text tables as dropdowns, numbers, text and hex-byte fields.
- **Service jobs window**: the line's guided jobs; everything the user must choose is asked in one dialog, the
  safety notes are shown, then one button runs every step (session, unlock, run, follow, plain-language result).
- **Session trace** of every request/response (`PorscheTool_trace_<day>.ptrace`), replayable with `--replay`.
- **CLI (Windows exe)**: `--scan`, `--clear-all`, `--vehicle <ID|auto>`, `--run <module> "<function>" [inputs...]`,
  `--job <id> [values...]`, `--demo`, `--replay <trace>`, `--redact-vin`.
- **porschetool-cli** (Linux/macOS): `vehicles`, `detect`, `modules`, `functions`, `jobs`, `scan`, `clear-all`,
  `faults`, `clear`, `run`, `live`, `job` over `--can <iface>`, `--demo` or `--replay`.

### Tested without a car

`./build.sh` runs: `tests/test_core.cpp` (fault parsing, status, VIN), `tests/test_pack.cpp` (request build +
response decode against a real pack, plus a sweep over every service), `tests/test_engine.cpp` (the whole engine
against the simulated car: scan with environment and snapshot data, HTML report, replay of the recorded session,
VIN and part-number detection, routine following, coding write with SecurityAccess 27 03/04, wrong key rejected
and not retried, KWP 27 01/02 with a 2-byte seed, guided jobs, live data). `tests/mock_j2534.cpp` puts the same
simulated car behind the real J2534 API (CAN and K-line) so `PorscheTool.exe` is tested end-to-end under Wine.

## Vehicle coverage (data packs)

All 13 lines: see [COVERAGE.md](COVERAGE.md) (generated by `tools/coverage.py`). Older lines (9x6, E1, part of 9x7)
are K-line units whose addresses come from the vehicle-information database (`*.vi.db`, `odxdb.logical_links`).
A line's directory must hold **both** string tables (`AStringData` and `UStringData`); without `UStringData` the
long names are missing and the parameter parser silently loses most of the requests (this broke the first E3 pack).

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
- `src/engine.h` — `securityUnlock()` runs 0x27 at the coding level the pack gives for the unit (`sa`: request-seed
  sub-function and seed length from the database), computes the key and sends it. It is called before a bit-8
  coding write and on an NRC 0x33. Nothing is sent without a mapped algorithm, with an unexpected seed length or
  without the key file; a rejected key is reported and never retried. The exchange is verified against the
  simulated control units (UDS 4-byte seed at 27 03/04, KWP2000 2-byte seed at 27 01/02, wrong key, attempt
  counter); the real-car confirmation is on the roadmap. SFD lines: see [SFD.md](SFD.md).

## TODO (next sessions)

See [ROADMAP.md](../ROADMAP.md).
