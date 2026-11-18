# PorscheTool roadmap

## Goal

**PorscheTool must be a complete replacement for the Porsche dealer tester (PIWIS).** Free and open source, for
independent workshops and owners — right to repair. Everything a workshop does with PIWIS on a car — read and
clear faults with their environment data, live data, identification, actuator tests, routines and basic settings,
adaptation and coding, and the guided service procedures — on every control unit of every Porsche line, over a cheap
J2534 cable (or SocketCAN on Linux), driven by data extracted from a local PIWIS install. Every item below serves
that goal; the work is not finished until a workshop never has to reach for PIWIS for a repair.

## Where we are (v0.5)

- **All 13 vehicle lines** have data packs: G1, G2, 9x1, CS1, 918S, 992, Cajun, E2, E3, J1, 9x7, 9x6, E1 — see
  [docs/COVERAGE.md](docs/COVERAGE.md) for control units, functions and jobs per line.
- Scan / clear all control units, with **fault environment data** (frequency, mileage, time stamp, voltage) and
  **snapshot records**, full status-byte detail; text + printable **HTML report** with optional VIN redaction.
- **Guided one-click jobs**: service reset, brake bleeding, parking-brake pad change, ride-height and level-sensor
  calibration, steering-angle and sensor-cluster calibration, engine adaptation reset, battery registration —
  offered per line where the database has the function.
- **Vehicle auto-detection** from the VIN (with part-number fallback); the user never has to pick the car.
- Live data of several values at once with a **graph** and CSV log; **session record / replay** for diagnosis
  and testing without a car.
- Coding unlock: seed/key exchange per control unit at the level the database names, verified against the
  simulated control units for UDS (4-byte) and KWP2000 (2-byte); a wrong key is never retried.
- **K-line** (ISO 14230, fast and 5-baud init) for the older lines; 11- and 29-bit CAN.
- Portable engine (`src/engine.h`) shared by the Windows app and a **command-line tool for Linux/macOS**
  (SocketCAN on Linux).
- Tested without a car: unit tests, a sweep over every function of every pack, the engine against a simulated car,
  and the Windows exe end-to-end under Wine against a mock J2534 adapter (CAN and K-line).

## Next — ranked

### 1. On-car confirmation (needs a car / bench)
- [ ] Spot-check each line's CAN ids and a few services against a real car or a known-good log (a recorded session
      from the app is enough: `PorscheTool_trace_*.ptrace`).
- [ ] Confirm the `0x27` exchange on real control units (protocol plumbing is verified on the simulator, the key
      maths against the PIWIS vectors) — PASM / cluster (UDS) and PDK (KWP2000) first.
- [ ] Confirm the K-line init and timing on a 996/986 and a Cayenne 955.
- [ ] Run each guided job once on a car of each line and adjust its wording from what the car shows.

### 2. Coding coverage
- [ ] Extend `seedkey_map.py` past the 36 high-confidence units as each is confirmed (never map on a guess).
- [ ] Map the classic seed/key units of 992 / J1 / E3 / G2 that do not use SFD.

### 3. Sub-bus control units
- [ ] VW TP 2.0 / TP 1.6 transport for the units behind the gateway that the packs list as "sub-bus only"
      (doors, seats, trailer hitch, auxiliary heater on G1/E2 ...).

### 4. More guided jobs
- [ ] Add rules to `tools/jobs.py` for further procedures as they are confirmed: PDCC fill and bleed, headlight
      basic setting, ACC / camera alignment, TPMS learning, climate flap addressing, A/C compressor run-in.

### 5. Translation quality
- [ ] Hand-check the machine translation of the most-used texts (fault texts of engine, PDK, PSM, PASM, cluster)
      in `tools/i18n/`.

### 6. Distribution
- [ ] Publish `porsche-tool` as a public repo: banner, README, release of `PorscheTool.exe`, the CLI and the English
      packs — same redaction rules as the pdk tool (no PIWIS texts, VINs or unlock constants). Needs the owner's go.
- [ ] macOS: a J2534-equivalent driver path (e.g. a USB CAN adapter through a user-space driver); today macOS runs
      the CLI in demo / replay mode only.

## Limits that are not ours to lift
- **SFD (online authentication)** on newer 992 / Taycan / 9Y0 / 971 control units: coding writes need the
  manufacturer's server — see [docs/SFD.md](docs/SFD.md). Reading, live data, tests, routines and jobs work offline.

## Design rules (keep)
- Dead-simple UI: a few big buttons that each do the whole job; raw/granular controls only in an "Advanced" area.
- Plain-language log. Safety confirmations (with the hazard notes) on anything that changes or moves something.
- Nothing from a local PIWIS install (original German texts, VINs, unlock constants) ever ships in the repo.
- Never send a guessed key. Never retry a rejected one.
