<p align="center">
  <img src="https://img.shields.io/badge/platform-Windows-0078D6?style=flat-square&logo=windows&logoColor=white" alt="Windows">
  <img src="https://img.shields.io/badge/interface-J2534%20(Tactrix%20OpenPort%202.0)-333?style=flat-square" alt="J2534">
  <img src="https://img.shields.io/badge/protocols-UDS%20·%20KWP2000%20on%20CAN-333?style=flat-square" alt="UDS / KWP2000">
  <img src="https://img.shields.io/badge/vehicle-Panamera%20970-333?style=flat-square" alt="Panamera 970">
  <img src="https://img.shields.io/badge/license-MIT-555?style=flat-square" alt="MIT">
  <img src="https://img.shields.io/badge/status-not%20yet%20tested%20on%20a%20car-orange?style=flat-square" alt="Status">
</p>

# PorscheTool – open-source Porsche diagnostics

**Read and clear the fault memory of every control unit, with English fault descriptions. One small Windows exe and a
J2534 cable.** Built for independent workshops and owners: the right to repair your own car.

Version 0.1 covers the **Panamera 970 (2010–2016)**: 32 control units on the diagnostic CAN, 8,752 fault codes with
English text.

> [!IMPORTANT]
> **Status: not yet tested on a car.** The protocol layer is tested end-to-end against a simulated adapter (see
> [Testing](#testing)). The CAN addresses and fault catalogues come from the car line's diagnostic database. Please
> report results: open an issue and attach the scan report.

## Features

| | | |
|---|---|---|
| 🔎 | **Scan all modules** | Every control unit: present or not, part number / software / hardware, all faults with English text and status (active now / stored / pending, warning lamp). |
| 🧹 | **Clear all faults** | One click, with confirmation. Opens the right session per module if the unit asks for it. |
| 🎯 | **One module** | Read or clear a single control unit. |
| 📄 | **Report** | Every scan is saved as a text file next to the exe. |
| 🔀 | **UDS + KWP2000** | Detects the protocol per unit and falls back automatically (the 970 mixes both). |
| ⌨️ | **Command line** | `PorscheTool.exe --scan` scans, saves the report and exits, for scripted use. |

### Control units (Panamera 970)

Engine (DME) · PDK · gear selector · PSM · electric parking brake · PASM / air suspension · PDCC · power steering ·
steering column switch · tyre pressure · airbag · gateway · instrument cluster · body control front / rear ·
headlights · climate · tailgate · park assist · ACC · front camera · rear / top view camera · lane change assist ·
Sport Chrono · head unit (PCM) · amplifier · TV tuner · hybrid battery / charger / e-motor / e-A/C compressor.

Seats, doors, trailer hitch, auxiliary heater and rear diff lock sit on sub-buses. They are listed in the report but
not scanned yet.

## Quick start

1. Install the driver for your J2534 adapter (Tactrix: *OpenPort 2.0 J2534 drivers*).
2. Connect the adapter to the OBD port. Ignition **ON**, engine off. A battery charger is recommended.
3. Start `PorscheTool.exe`. The status line turns green: **Adapter: CONNECTED**.
4. Press **SCAN ALL MODULES**. The report `PorscheTool_scan_<date>.txt` is saved next to the exe.

## Roadmap: service functions

The next releases add the workshop procedures that today need a dealer tester. These include air-suspension height
calibration, parking-brake service position, brake bleeding with the ABS pump, PDCC fill and bleed, headlight and ACC
calibration, tyre-pressure sensor learning and the service reset. The full catalogue, with the request IDs per control
unit, is in [docs/SERVICE_FUNCTIONS_970.md](docs/SERVICE_FUNCTIONS_970.md), together with what is deliberately out of
scope: odometer, immobiliser, crash data and high-voltage safety functions.

PDK teach-in and gearbox diagnostics are already available in the sister project
[pdk-3000-tool](https://github.com/brstic/pdk-3000-tool).

## Building from source

```sh
./build.sh          # runs the unit tests, then builds PorscheTool.exe (32-bit, static) with mingw-w64
```

32-bit on purpose: most J2534 drivers are 32-bit DLLs.

### Regenerating the module table

`src/vehicle_g1.h` is generated from a local installation of the manufacturer's diagnostic database, which you must
own yourself. It is not included here.

```sh
python3 -I tools/build_vehicle.py <…/Projects-V/G1> G1 src/vehicle_g1.h
```

The generator reads each control unit's CAN request/response IDs, protocol and fault catalogue. The English texts come
from `tools/i18n/dtc_en.json`, keyed by a hash of the original text, so this repository contains no original database
texts.

## Testing

- `tests/test_core.cpp`: response parsing, code formatting, catalogue lookups and table sanity, on any OS.
- `tests/mock_j2534.cpp`: a fake `op20pt32.dll` that simulates a PDK (KWP, clear needs session 0x89), PASM (UDS), an
  airbag unit, a gear selector that refuses UDS, and an engine unit that answers "response pending". Put it next to
  `PorscheTool.exe` (for example under Wine) and run `PorscheTool.exe --scan` or `--clear-all`.

## Repository layout

```
src/porschetool.cpp        Windows app: J2534, UDS/KWP transport, scan/clear, GUI
src/diag_core.h         protocol-independent parsing and formatting (unit-tested)
src/vehicle_g1.h        generated module table + English fault catalogue (Panamera 970)
tools/build_vehicle.py  generator (reads your local diagnostic database)
tools/piwis_db.py       reader for the database file format
tools/i18n/dtc_en.json  English fault texts
tests/                  unit tests and the mock J2534 adapter
docs/                   service-function catalogue
```

## Disclaimer

PorscheTool is an independent project. It is not affiliated with, endorsed by or connected to Porsche AG. "Porsche",
"Panamera", "PDK", "PASM", "PDCC" and "PSM" are trademarks of their owner and are used only to describe compatibility.
Use at your own risk. Write down the faults before you clear them, and do not run tests while driving.
