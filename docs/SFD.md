# Coding unlock: what works offline and what does not

OWIS needs a security unlock only for **coding / adaptation writes** and a few actuator tests. Reading
fault memory, clearing it, live data, identification, most actuator tests and routines need no unlock on any
generation.

## Older generations: seed / key, computed locally

Panamera 970, 911 991, Boxster/Cayman 981/718, 918, Macan 95B, Cayenne 92A and the older lines use the classic
UDS / KWP2000 *SecurityAccess* exchange (service `0x27`):

1. the tool asks the control unit for a **seed** at the coding level the data pack names for that unit
   (`27 03` on most UDS units, `27 01` on KWP2000 units such as the PDK),
2. computes the **key** with the algorithm mapped to that unit,
3. sends the key (`27 04` / `27 02`). The unit answers `67 ..` and accepts the write.

The algorithms and their constants are **not** part of this repository or of the data packs. They are read from a
local key file (`owis.keys`) that you generate from your own diagnostic installation with
`tools/build_keyfile.py`. Without the key file everything except coding writes still works.

Safety rules built into the tool — a wrong key costs one of the unit's unlock attempts and can lock it for a while:

- a key is only sent when the unit is **mapped** to an algorithm (`tools/seedkey_map.py`, high-confidence entries
  only) and the seed length the unit sends **matches** the algorithm;
- a rejected key is reported and **never retried**; the tool does not try a second security level after a rejection;
- with no mapping, no key file or an unexpected seed, nothing is sent at all and the log says why.

## Newer generations: SFD, online only

From roughly model year 2020 the Volkswagen group protects coding with **SFD** (*Schutz Fahrzeugdiagnose*, vehicle
diagnostic protection): the control unit issues a challenge, and the unlock token is a **signature created by the
manufacturer's server** for that one car and session. There is no local algorithm to compute — this is by design
and no offline tool can do it.

What that means in OWIS:

- Units whose database defines the SFD routines (`…protection_of_vehicle_diagnosis`, service `0x29`) are marked
  per unit (`sfd` in the module); those routines themselves are not offered. In addition the whole line is marked
  (`sfd` in `tools/platforms.py`), so on 992 / Taycan / 9Y0 / 971 a coding write on a unit without a mapped seed/key
  algorithm is not attempted; the app says that coding there needs the online authentication.
- 911 **992**, **Taycan**, Cayenne **9Y0** and the facelift Panamera **971** are affected, depending on model year
  and control unit. Units of those cars that still use a classic seed/key are simply not mapped yet (see above —
  nothing is mapped on a guess).
- Everything else on these cars works offline: scan and clear all control units, fault environment data, live
  data and graphs, identification, actuator tests, routines and the guided jobs that are routines (service reset,
  brake bleeding, parking-brake pad change ...).

Access to SFD is a matter between the vehicle owner or workshop and the manufacturer (independent-operator
programmes exist for this). OWIS does not implement the online exchange and does not try to get around it.
