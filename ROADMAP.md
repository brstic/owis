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
- **K-line** (ISO 14230, fast and 5-baud init) for the older lines; 11- and 29-bit CAN; **VW TP 2.0** for the units
  behind the gateway (doors, seats, auxiliary heater ...).
- **PDK / transmission**: oil fill mode, teach-ins with live progress and the unmet conditions, adaptation reset —
  as in PdkTool, on every line with a PDK.
- Portable engine (`src/engine.h`) shared by the Windows app and a **command-line tool for Linux/macOS**
  (SocketCAN on Linux, SLCAN serial CAN adapters on macOS and Linux).
- Tested without a car: unit tests, a sweep over every function of every pack, the engine against a simulated car,
  and the Windows exe end-to-end under Wine against a mock J2534 adapter (CAN and K-line).

## Next — ranked

### 1. On-car confirmation (needs a car / bench)
- [ ] Spot-check each line's CAN ids and a few services against a real car or a known-good log (a recorded session
      from the app is enough: `PorscheTool_trace_*.ptrace`).
- [ ] Confirm the `0x27` exchange on real control units (protocol plumbing is verified on the simulator, the key
      maths against the PIWIS vectors) — PASM / cluster (UDS) and PDK (KWP2000) first.
- [ ] Confirm the K-line init and timing on a 996/986 and a Cayenne 955.
- [ ] Confirm the VW TP 2.0 addresses of the sub-bus units (standard VW addresses, marked "not yet confirmed" in the app).
- [ ] Run each guided job once on a car of each line and adjust its wording from what the car shows.

### 2. Coding coverage
- [x] The coding algorithm of every unit is now read from the database itself (the SeedKey job of the Codierung
      level names it - the same reference PIWIS uses), so nothing is mapped on a guess; `seedkey_map.py` only keeps
      a few hand overrides (hardware-variant names). Engine control units are deliberately left without a key
      (calibration / emissions-relevant changes are out of scope) and their calibration-data writes are not offered.
      Checking the old hand table against the database corrected the instrument cluster of 970 / 918 / 92A (database:
      `ASAM_Seedkey_Ki`, not the 991 cluster algorithm) and the rear differential lock (database: no real coding key).
- [ ] 992 / J1 / E3 / G2: their databases name no offline coding algorithm (SFD) - nothing to map offline.
- [ ] Confirm the unlock on a real unit of each algorithm family (part of section 1).

### 3. Sub-bus control units — done
- [x] VW TP 2.0 transport (`src/tp20.h`): channel setup, parameters, numbered frames with ACK, keep-alive. Doors,
      seats, auxiliary heater, trailer hitch, rear camera and lane-change assistant of 970 / 92A / 95B / 991 are scanned
      and fully usable. J2534 (raw CAN), SocketCAN and SLCAN; tested frame by frame against the simulator and under Wine.
- [ ] Rear differential lock (Quersperre): TP 2.0 address unknown — needs a car.

### 4. More guided jobs — done
- [x] PDK: oil fill mode, teach-in shift rods / clutches / hydraulics (with the unmet conditions listed on failure),
      adaptation reset; Tiptronic adaptation reset.
- [x] PDCC fill and bleed, headlight range basic setting, headlight axle-sensor calibration, climate flap motor
      addressing, A/C compressor run-in reset, TPMS: take target pressures / write target pressures.
- [ ] Further procedures as they are confirmed on a car (ACC / camera alignment need targets and stay manual functions).

### 5. Translation quality — done for the main units
- [x] Fault texts of engine, PDK, PSM, PASM and cluster checked (hand translations); glossary extended and the cached
      machine translation refreshed where the glossary applies; technical identifiers shown as readable words;
      readable control-unit names on every line.
- [ ] Ongoing: hand-fix wrong translations as users report them (`tools/i18n/*.json`).

### 6. Distribution
- [x] macOS and Linux: `porschetool-cli --slcan <port>` for serial CAN adapters (CANable, CANtact, USBtin ...), with
      ISO-TP and TP 2.0 in user space; Linux also `--can <iface>` (SocketCAN). Tested through a pseudo-terminal.
- [ ] Publish `porsche-tool` as a public repo: banner, README, release of `PorscheTool.exe`, the CLI and the English
      packs — same redaction rules as the pdk tool (no PIWIS texts, VINs or unlock constants). **Needs the owner's go.**

## Limits that are not ours to lift
- **SFD (online authentication)** on newer 992 / Taycan / 9Y0 / 971 control units: coding writes need the
  manufacturer's server — see [docs/SFD.md](docs/SFD.md). Reading, live data, tests, routines and jobs work offline.

## Design rules (keep)
- Dead-simple UI: a few big buttons that each do the whole job; raw/granular controls only in an "Advanced" area.
- Plain-language log. Safety confirmations (with the hazard notes) on anything that changes or moves something.
- Nothing from a local PIWIS install (original German texts, VINs, unlock constants) ever ships in the repo.
- Never send a guessed key. Never retry a rejected one.
