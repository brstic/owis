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

Built & verified: **G1** (Panamera 970), **9x1** (911 991 / 981 / 718), **CS1**, **918S**, **992** (911 992, ODX 2.2).
In progress / next: **Cajun** (Macan), **E2/E3** (Cayenne), **J1** (Taycan), **MIG/9x7/9x6/E1** (997/987, 996/986,
Cayenne 955). Each pack ≈ 10k–22k services across 40–68 control units. KWP lines (G1/9x1/918) mix UDS+KWP; 992/newer
are UDS with 29-bit addressing on many modules.

## Seed/key (coding unlock) — see §14 of PDK_TCU_KNOWLEDGE.md

Needed only for coding **writes** (0x2E) and a few actuator tests; reading, routines and most tests need no key.
87 module jars catalogued (local, `~/Projects/porsche/seedkey/`): 66 solved — **keyed-multiply**
`key=((seed^K)*A+B)` (57) and **2-byte rotate table** (9). 21 array-hash jars (EPB, BDE, ACC3, Lift_VA, SWSG,
kombi, …) still to port from bytecode. 992+ uses online SFD, not computable offline.

## TODO (next sessions)

1. Build packs for the remaining lines (Cajun, E2, E3, J1, 9x7/9x6/E1).
2. Port the 21 remaining array-hash seed/key algorithms; wire seed/key into the app so coding writes work; map each
   module to its seed/key algorithm (from the DB's SecurityAccess job reference).
3. Publish `porsche-tool` as a public repo (banner, README, release) — same redaction rules as pdk-3000-tool.
4. Freeze-frame / environment data decoding for UDS DTCs (SDG files hold the system reaction per fault).
5. Per-module safety notes for the risky write/actuator functions.
