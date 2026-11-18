<p align="center">
  <img src="https://img.shields.io/badge/platform-Windows%20·%20Linux-0078D6?style=flat-square" alt="Windows / Linux">
  <img src="https://img.shields.io/badge/interface-J2534%20·%20SocketCAN-333?style=flat-square" alt="J2534 / SocketCAN">
  <img src="https://img.shields.io/badge/protocols-UDS%20·%20KWP2000%20·%20CAN%20·%20K--line-333?style=flat-square" alt="Protocols">
  <img src="https://img.shields.io/badge/vehicles-13%20Porsche%20lines-333?style=flat-square" alt="13 lines">
  <img src="https://img.shields.io/badge/license-MIT-555?style=flat-square" alt="MIT">
  <img src="https://img.shields.io/badge/status-not%20yet%20tested%20on%20a%20car-orange?style=flat-square" alt="Status">
</p>

# PorscheTool – open-source Porsche diagnostics

**A free replacement for the dealer tester, for independent workshops and owners — right to repair.** Every control
unit of every Porsche line from the 996 to the Taycan: faults with their environment data, live data and graphs,
identification, actuator tests, routines and basic settings, adaptation and coding, and one-click service jobs.
One small Windows exe (or a Linux command-line tool) and a cheap cable.

> [!IMPORTANT]
> **Status: not yet tested on a car.** Everything is tested end-to-end against a simulated car and a simulated
> J2534 adapter (see [Testing](#testing)). Addresses, functions and fault catalogues come from each line's diagnostic
> database. Please report results — a recorded session (`PorscheTool_trace_*.ptrace`) is the most useful thing to send.

## What it does

| | | |
|---|---|---|
| 🚗 | **Knows the car** | Reads the VIN and picks the right vehicle data by itself. |
| 🔎 | **Scan all modules** | Every control unit: part number / software / hardware, all faults in plain English with status, how often and when each fault was stored (mileage, date, voltage) and the snapshot values. |
| 🧹 | **Clear all faults** | One click, with confirmation. |
| 🛠️ | **Service jobs** | Service reset, brake bleeding with the ABS pump, parking-brake pad change, ride-height and level-sensor calibration, steering-angle and sensor-cluster calibration, engine adaptation reset, battery registration — one button each, from session to result. |
| 📈 | **Live data** | Several values at once, with a graph and a CSV log. |
| 🧪 | **Every function** | All actuator tests, routines, adaptation and coding values the control unit's database defines, with safety notes before anything moves. |
| 📄 | **Reports** | Text and printable HTML report of every scan, VIN can be hidden. |
| 🎞️ | **Record / replay** | Every session is recorded and can be replayed later without the car. |

Vehicle lines: Panamera 970 and 971 · 911 996, 997, 991, 992 · Boxster / Cayman 986, 987, 981, 718 · 918 Spyder ·
Macan · Cayenne 955/957, 92A, 9Y0 · Taycan · Clubsport race cars. Per-line detail in
[docs/COVERAGE.md](docs/COVERAGE.md).

Not offered, on purpose: odometer changes, immobiliser / key learning / component protection, airbag crash data,
high-voltage safety disable, emissions-relevant engine changes. Coding on newer cars that use online authentication
(SFD) is explained in [docs/SFD.md](docs/SFD.md).

## Quick start (Windows)

1. Install the driver for your J2534 adapter (Tactrix: *OpenPort 2.0 J2534 drivers*).
2. Put `PorscheTool.exe` and the `data` folder in one folder.
3. Connect the adapter to the OBD port. Ignition **ON**, engine off. A battery charger is recommended.
4. Start `PorscheTool.exe`. The status line turns green and the car is recognised by itself.
5. Press **SCAN ALL MODULES**, or **SERVICE JOBS** for a procedure. Reports are saved next to the exe.

`PorscheTool.exe --demo` runs everything against a simulated Panamera — no car or adapter needed.

## Linux / macOS

```sh
./build.sh                                   # builds build/porschetool-cli (and the Windows exe if mingw is there)
sudo ip link set can0 up type can bitrate 500000
build/porschetool-cli --can can0 scan        # any SocketCAN adapter (candleLight, CANable, PEAK, Kvaser ...)
build/porschetool-cli --can can0 jobs
build/porschetool-cli --demo scan            # simulated car, any OS
```

## Building from source

```sh
./build.sh          # runs all tests, then builds PorscheTool.exe (32-bit, static) with mingw-w64 and the CLI
```

32-bit on purpose: most J2534 drivers are 32-bit DLLs.

### Vehicle data

The data packs are generated from a local installation of the manufacturer's diagnostic database, which you must
own yourself; it is not included here. `tools/make_packs.py` builds all lines (German texts → offline machine
translation → English pack). The English texts are keyed by a hash of the original text, so no original database
text is stored in this repository. Details in [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## Testing

`./build.sh` runs, without a car: unit tests; a sweep that builds the request and decodes a response for every
function of a pack; the whole engine against a simulated car (scan with environment and snapshot data, report,
replay, vehicle detection, routines, coding write with security access, wrong-key handling, guided jobs, live
data). The mock adapter `build/op20pt32.dll` puts the same simulated car behind the J2534 API (CAN and K-line), so
`PorscheTool.exe` itself is tested end-to-end under Wine.

## Disclaimer

Not affiliated with or endorsed by Porsche AG. Use at your own risk and follow the workshop manual for every
procedure. MIT license.
