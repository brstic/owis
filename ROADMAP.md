# PorscheTool roadmap

Goal: a complete, free, right-to-repair replacement for the Porsche dealer tester (PIWIS) for an independent
workshop and an owner — read/clear faults, live data, identification, actuator tests, routines/basic settings
and adaptation/coding, on every control unit, over a cheap J2534 cable, driven by data extracted from a local
PIWIS install.

**Out of scope, permanently:** odometer writes, immobiliser / key learning / component protection, airbag
crash-record read or manipulation, high-voltage-battery safety disable, emissions-relevant engine changes.
These are deliberately excluded from the packs and the tooling.

## Where we are

- Data packs built & verified: **G1, 9x1, CS1, 918S, 992, Cajun, E2, E3** (8 lines, ~8k–22k services each).
- Scan/clear all, per-module live data, identification, actuator tests, routines, adaptation read, coding write.
- Seed/key coding unlock wired in: 7 algorithm families, 36 mapped modules, key maths verified against the
  PIWIS vectors. Local key file only; nothing secret in the repo.
- Tested without a car via `tests/` + the mock J2534.

## Next — ranked

### 1. Finish vehicle coverage
- [ ] **J1** (Taycan) — pack build + verify (EV modules, 29-bit UDS).
- [ ] **G2** (Panamera 971) — pack.
- [ ] **MIG/9x7, MIG/9x6, MIG/E1** (997/987, 996/986, Cayenne 955) — older KWP-heavy lines.
- [ ] Spot-check each new pack's CAN ids and a few services against the car or a known-good log.

### 2. Confirm seed/key on the bench/car
- [ ] Exercise the `0x27` request-seed → compute → send-key exchange against the mock (add a SecurityAccess
      module to `tests/mock_j2534.cpp`) and then a real control unit. The key *maths* is already verified; this
      is the protocol plumbing (level selection, byte offsets, KWP vs UDS).
- [ ] Extend `seedkey_map.py` past the 36 high-confidence modules as each is confirmed (never map on a guess —
      a wrong key can trip a control unit's attempt counter).

### 3. Freeze-frame / environment data for faults
- [ ] Decode the UDS DTC snapshot (`19 04`) and extended data (`19 06`) records — the conditions captured when
      a fault set (RPM, temp, speed, voltage…). The record layout is in the ODX `ENV-DATA` / SDG structures;
      extend `odxdb.py` to emit it and `pack.h` to decode it. Show it under each fault.
- [ ] DTC status byte breakdown (pending / confirmed / test-failed / aged) and the fault occurrence counter.

### 4. Guided one-click jobs (the UX priority)
Wrap raw services into a few big end-to-end tasks a mechanic runs without chaining steps:
- [ ] Service reset / oil service reset.
- [ ] Brake bleed / ABS bleeding routine.
- [ ] EPB (electric parking brake) service / pad-change mode.
- [ ] Suspension ride-height calibration (PASM/air).
- [ ] Throttle-body / engine adaptation reset.
- [ ] Battery registration (where coding, not component protection).
- [ ] Steering-angle sensor calibration.
Each: auto-select the module, enter the right session, unlock if needed, run, show plain-language status.

### 5. Vehicle auto-detection
- [ ] Read VIN (`22 F190`) and model/line, auto-pick the data pack, so the user never chooses manually.
- [ ] Fall back to a scan-and-identify when the VIN is unavailable.

### 6. Reporting & live data
- [ ] Export a scan/clear report as HTML/PDF (fault list + identification, VIN redactable).
- [ ] Live-data graphing (plot a selected value over time) and multi-value dashboards.
- [ ] Session record / replay for diagnosis and for testing without a car.

### 7. Platform & distribution
- [ ] Native Linux/macOS build path (SocketCAN or a cross-platform J2534 shim) — today the app is Win32-only.
- [ ] Publish `porsche-tool` as a public repo: banner, README, release of `PorscheTool.exe` + English packs,
      same redaction rules as the pdk tool (no PIWIS texts/VINs, no unlock constants).
- [ ] Per-module safety notes for the risky write/actuator functions, shown before running them.

### 8. 992+ online unlock (document the limit)
- [ ] 992 and newer use online SFD ("Signatur" / SFD Stufe 2), not an offline seed/key. Document clearly that
      coding writes on those lines need the online path; keep read/scan/routines fully working offline.

## Design rules (keep)
- Dead-simple UI: a few big buttons that each do the whole job; raw/granular controls only in an "Advanced" area.
- Plain-language log. Safety confirmations on anything that changes a control unit.
- Nothing from a local PIWIS install (original German texts, VINs, unlock constants) ever ships in the repo.
