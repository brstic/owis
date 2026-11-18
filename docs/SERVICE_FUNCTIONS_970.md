# Panamera 970 – workshop service functions (catalogue)

What an independent workshop needs after common repairs, and how each control unit exposes it. Requests are
UDS (ISO 14229) unless marked KWP. `31 01 xxxx` = start routine, `31 03 xxxx` = read routine result,
`2E/22 xxxx` = write/read data identifier, `2F xxxx` = input/output control.

> [!NOTE]
> **Status:** this is the map for the next OpenDiag releases. The IDs come from the diagnostic database of the car
> line. The routine option bytes, result layouts and preconditions still have to be decoded, and every function has
> to be checked on a car before the tool offers it. The **SA** column shows that the unit defines SecurityAccess
> levels. Whether a given routine needs unlocking is not yet confirmed.

| Job (after which repair) | Module (CAN req/resp) | Request(s) | SA |
|---|---|---|---|
| **Air suspension – ride-height calibration** (strut, height sensor, control unit replaced) | PASM 0x755/0x7BF | write height per corner `2E A0..A3`, store `2E A4`; level the car `31 01 F105` | 27 03/04 |
| **Air suspension – fill / vent air springs** (strut or compressor work) | PASM | fill system `31 01 F101`, fill from reservoir `31 01 F102`, vent to reservoir `31 01 F103`, vent component `31 01 F104` | 27 03/04 |
| **Air suspension – lock/unlock levelling** (lift, wheel alignment) | PASM | `31 01 F106` | 27 03/04 |
| **Air suspension – actuator tests** (compressor, valves, dampers, extra volume) | PASM | `31 01 F10B`, `F110`, `F111`, `F112`; height sensor test `F10C`; system air check `F10A` | 27 03/04 |
| **Parking brake – service position / calibration** (brake pads, discs, EPB motor) | EPB 0x752/0x7BC | calibration `31 01 0410` (+ `2F 0410` short-term adjustment); bedding-in mode `31 01 0409`; actuator counter reset `31 01 F021` | 27 03/04 |
| **Brake bleeding with the ABS pump** (master cylinder, ABS unit, lines) | PSM 0x713/0x77D | evacuate & fill `31 01 04`, rework bleeding `31 01 05` | 27 03/04 |
| **Yaw/steering sensor cluster zero** (PSM unit or sensor replaced, alignment) | PSM | sensor cluster calibration `31 01 09` | 27 03/04 |
| **Wheel speed sensor test** | PSM | `31 01 08`; static/dynamic test `31 01 07` / `31 01 06` | 27 03/04 |
| **Dyno / roller mode** | PSM | `31 01 0A` | 27 03/04 |
| **PDCC – fill & bleed hydraulics** (PDCC pump, valve block, actuator, lines) | PDCC 0x722/0x78C | line fill `31 01 F008`, bleeding `31 01 F009`; pressure sensor calibration `2E 0601`; system / standing / roll test `F006`, `F005`, `F00E` | 27 03/04 |
| **Headlight range control – basic setting** (headlight, levelling sensor, alignment) | Headlights 0x725/0x78F | calibration `31 01 0401`, reference run `31 01 0402`; sensor zero front/rear `2E 04B2` / `2E 04B1` | 27 03/04 |
| **Axle sensor calibration for beam levelling** (cars where the BCM reads the sensor) | BCM front 0x70E/0x778 | `31 01 0401` | 27 03/04 |
| **ACC radar alignment** (bumper, radar, bracket) | ACC 0x757/0x7C1 | alignment `2F 0302`, status/screw guidance `22 0501..0504`, misalignment reset `2E 256C` | 27 03/04 |
| **Front camera calibration** (windscreen, camera) | Front camera 0x74F/0x7B9 | board distance `2E 3BAF`, result `22 1519` | 27 03/04, 27 21/22 |
| **Tyre pressure – learn wheels / accept pressures** (new wheels or sensors) | TPMS 0x70B/0x775 | write wheel IDs `2E 03A3` (RDK) / `2E 091B` (RDK3); take target pressures from cluster `31 01 0412`; reset adaptation `31 01 0317` | 27 03/04 |
| **Service interval reset** (oil service, inspection) | Cluster 0x714/0x77E | `31 01 0450`; counters `22/2E 0351..0354` | 27 03/04 |
| **Climate – flap motor addressing** (flap motor or climate unit replaced) | Climate 0x746/0x7B0 | LIN motor auto-addressing `31 01 0401` / `0406` | 27 03/04 |
| **A/C compressor run-in timer** (new compressor) | Climate | remaining time `22 0320`, reset `31 01 040A` | 27 03/04 |
| **Power steering self-test / actuator test** | Power steering 0x712/0x77C | `31 01 01` / `31 01 00` | 27 03/04 |
| **PDK teach-in** (TCU, mechatronic, clutch) | PDK 0x70F/0x779 (KWP) | already in [PdkTool](https://github.com/brstic/pdk-3000-tool) | – |

## Deliberately out of scope

These exist in the database. OpenDiag will **not** implement them: they are anti-theft, odometer, crash-data or
high-voltage safety functions, not repair procedures.

- Instrument cluster odometer writes, immobiliser / key learning, KESSY and BCM crypto resets.
- Airbag crash-record initialisation and airbag control-unit lock/unlock.
- Hybrid battery: high-voltage deactivation mode and switching off insulation monitoring.
- Engine-control calibration changes (emissions).
