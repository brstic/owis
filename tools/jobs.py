#!/usr/bin/env python3
"""Guided one-click jobs: workshop procedures wrapped as a short list of control-unit functions.

Each job is a rule matched against the *source* names of a vehicle line's functions when its data pack is
built; the pack then carries only the result (module index, function index, input presets) plus the English
texts below. A job is offered for a line only when every non-optional step was found there, so the list of
jobs differs per line. Nothing here is guessed at run time: what a job sends is exactly the function the
database defines, with the inputs fixed below or chosen by the user.

step fields:
  mod    regex on the control-unit key            fn     regex on "<short name> <long name>" of the function
  sid    request service id(s) the function must use (0x31 routine start, 0x2E write, 0x3B KWP write ...)
  no     regex that must NOT match
  preset [(regex on an input's name, value)]: value for the matching inputs. A value can be a number or
           "$y" two-digit year, "$Y" year, "$M" month, "$D" day, "$ymd" 3-byte date (yy mm dd)  - today's date
           ("$opt", regex)  the option of a selection whose text matches
           "$only"          the single meaningful option of a selection (else the user chooses)
         Inputs without a preset are chosen by the user, all in one dialog before anything runs.
  need   regex one of the inputs must match (else the function is not what the job expects)
  max_ask  the step is only accepted when at most this many inputs are left for the user
  opt    True: step is skipped when the line does not have it
  label  plain-language name of the step
"""
import re

DATE = [(r"(?i)jahr|year", "$y"), (r"(?i)monat|month", "$M"), (r"(?i)\btag\b|\bday\b", "$D"), (r"(?i)datum|date", "$ymd")]
CLUSTER = r"Kombi|DashBoard|kombi"
SVC_RESET = r"Serviceintervall.?r(ü|ue)cksetzen|Service.?[Ii]nterval.?[Rr]eset|Reset.?[Ss]ervice.?[Ii]nterval"
NOT_NEW = r"(?i)\bneu\b|verl(ä|ae)ngerung|historie"

JOBS = [
    dict(id="oil_service_reset", title="Oil service reset",
         desc="Resets the oil-service reminder in the instrument cluster after an oil change. Today's date is "
              "stored as the service date.",
         steps=[dict(mod=CLUSTER, fn=SVC_RESET, no=NOT_NEW, sid=(0x31,), need=r"(?i)(ö|oe)l|oil",
                     preset=[(r"(?i)(ö|oe)l|oil", 1), (r"(?i)haupt|main|inspe[ck]t|zwischen|intermediate|WIV", 0)] + DATE,
                     label="Reset the oil-service interval")]),
    dict(id="service_reset", title="Service interval reset (choose which)",
         desc="Resets the service reminder in the instrument cluster after a completed service. Choose which "
              "service was done (oil service, intermediate or main service). Today's date is stored as the service date.",
         steps=[dict(mod=CLUSTER, fn=SVC_RESET, no=NOT_NEW, sid=(0x31,), preset=DATE, label="Reset the service interval")]),

    dict(id="brake_bleed_wheel", title="Brake bleeding with the ABS pump (one wheel / phase)",
         desc="Rework bleeding: the hydraulic unit runs its pump and valves so that air is pushed out of one brake "
              "circuit - needed after work on the hydraulic unit, master cylinder or brake lines. Choose the wheel "
              "(or phase) and run the job once for each, in the order the workshop manual gives.",
         steps=[dict(mod=r"^PSM$|ESP|Brake1|_psm$", fn=r"Nacharbeitsentl(ü|ue)ftung|RepairBleed|Rework.?[Bb]leed", sid=(0x31,),
                     label="Bleed with the hydraulic unit")]),
    dict(id="brake_bleed_sequence", title="Brake bleeding sequence (start / first / second / finish)",
         desc="The bleeding routine of the brake control unit, one phase per run: start, first bleeding, second "
              "bleeding, finish. Choose the phase the workshop manual asks for.",
         steps=[dict(mod=r"Brake1", fn=r"Bleed_brakes|Bleed brakes", no=r"(?i)repair|return", sid=(0x31,), label="Run the bleeding phase")]),

    dict(id="epb_pad_change_open", title="Parking brake: open for brake-pad change",
         desc="Puts the electric parking brake into pad-replacement mode: the brake motors wind fully back so the "
              "rear pads can be changed. Finish with 'Parking brake: close after brake-pad change'.",
         steps=[dict(mod=r"^EPB$|Brake1|ParkiBrake", fn=r"Start_?brake_?pad_?replacement_?mode|Start brake pad replacement", sid=(0x31,),
                     preset=[(r".", "$only")], max_ask=0, label="Start pad-replacement mode")]),
    dict(id="epb_pad_change_close", title="Parking brake: close after brake-pad change",
         desc="Ends pad-replacement mode: the parking brake motors move back to the new pads and the brake is "
              "ready again. Run it with the wheels fitted and the calipers bolted.",
         steps=[dict(mod=r"^EPB$|Brake1|ParkiBrake", fn=r"Complete_?brake_?pad_?replacement_?mode|Complete brake pad replacement", sid=(0x31,),
                     preset=[(r".", "$only")], max_ask=0, label="Complete pad-replacement mode")]),
    dict(id="epb_calibration", title="Parking brake: calibration after brake work",
         desc="Calibration run of the electric parking brake after work on the rear brakes or the parking-brake "
              "motors. The brake applies and releases by itself.",
         steps=[dict(mod=r"^EPB$", fn=r"\bKalibrierung\b", no=r"EOL|Lenk|H(ö|oe)he", sid=(0x31,), max_ask=0, label="Calibrate the parking brake")]),

    dict(id="ride_height_calibration", title="Air suspension: ride-height calibration",
         desc="Stores the measured ride height of each corner in the suspension control unit and saves the "
              "calibration - after a strut, height sensor or control unit was replaced. Measure the four heights "
              "as the workshop manual describes and enter them in mm.",
         steps=[dict(mod=r"^PASM$", fn=r"Kalibrierung\s+H(ö|oe)he vorne links", sid=(0x2E,), label="Height front left"),
                dict(mod=r"^PASM$", fn=r"Kalibrierung\s+H(ö|oe)he vorne rechts", sid=(0x2E,), label="Height front right"),
                dict(mod=r"^PASM$", fn=r"Kalibrierung\s+H(ö|oe)he hinten links", sid=(0x2E,), label="Height rear left"),
                dict(mod=r"^PASM$", fn=r"Kalibrierung\s+H(ö|oe)he hinten rechts", sid=(0x2E,), label="Height rear right"),
                dict(mod=r"^PASM$", fn=r"Kalibrierung speichern", sid=(0x2E,), preset=[(r".", 1)], label="Save the calibration")]),
    dict(id="level_sensor_calibration", title="Suspension: level-sensor calibration",
         desc="Calibrates the ride-height (level) sensors of the suspension control unit after a sensor, a "
              "suspension part or the control unit was replaced. The car must stand in the position the workshop "
              "manual prescribes.",
         steps=[dict(mod=r"PASM", fn=r"Niveausensoren.?Kalibrieren", no=r"au(ß|ss)erordentlich", sid=(0x31,), max_ask=0, label="Calibrate the level sensors")]),

    dict(id="steering_angle_calibration", title="Steering-angle sensor calibration",
         desc="Sets the zero point of the steering-angle sensor after work on the steering or a wheel alignment. "
              "Front wheels and steering wheel exactly straight ahead.",
         steps=[dict(mod=r"Lenkstockschalter|SteerColum", fn=r"Kalibrierung (des )?Lenkwinkelsensors?|Steering.?[Aa]ngle.*SET zero point$", no=r"Dekalib|Vorhalt|Continue|RESET",
                     sid=(0x31,), max_ask=0, label="Calibrate the steering-angle sensor")]),
    dict(id="sensor_cluster_calibration", title="PSM sensor cluster calibration (yaw rate / acceleration)",
         desc="Zero calibration of the yaw-rate and acceleration sensors of the stability management after the "
              "sensor or the control unit was replaced. The car stands level and absolutely still.",
         steps=[dict(mod=r"^PSM$", fn=r"Abgleich Sensorcluster", sid=(0x31,), max_ask=0, label="Calibrate the sensor cluster")]),

    dict(id="engine_adaptation_reset", title="Engine: reset adaptation values",
         desc="Resets the learned adaptation values of the engine control unit - after replacing the throttle "
              "body, injectors, air-mass or oxygen sensors. The engine relearns while driving; idle can be uneven "
              "at first.",
         steps=[dict(mod=r"ECM|DME|EnginContr|MED|_bde$|sdi41|med91|EDC17", fn=r"Reset of all Adaption Values|Reset of Adaption Values|Adaptionswerte[ _](zur(ü|ue)cksetzen|l(ö|oe)schen)",
                     no=r"(?i)injector|transmission|VTG|flap|clutch|Stop|Result", sid=(0x31,),
                     preset=[(r"(?i)^alle.?adapti|^all.?adapti", 1),
                             (r"(?i)RBM|Kraftstoff|LDR|Lade|lc_|DMTL|INTC|NMAX|Lambda|(ö|oe)l|Verlust|(ü|ue)berwachung", 0),
                             (r".", ("$opt", r"(?i)^all[ _]adapti"))], max_ask=0, label="Reset all adaptation values")]),

    dict(id="battery_registration", title="Battery change: register the new battery",
         desc="Tells the energy management that a new 12 V battery was fitted: capacity, technology and the "
              "battery's part / serial number from its label. Wrong data shortens battery life, so register only a "
              "battery that was really fitted.",
         steps=[dict(mod=r"Gateway|Gatew|DisplUnit", fn=r"Batteriewechsel Technologie zwei|Battery_Adaption|Battery Adaption", sid=(0x2E,),
                     label="Write the battery data")]),
]

def _sid(svc):
    rq = svc[4]
    return int(rq[0][4]) if rq and rq[0][4] is not None and rq[0][1] == 0 else None

def _const_at(svc, byte):
    for p in svc[4]:
        if p[1] == byte and p[4] is not None: return int(p[4])
    return None

def _addressable(svc, proto):
    """The function must be a real start / write with a known identifier - not a stop or result request, and
    not one whose routine or data identifier could not be resolved from the database."""
    sid = _sid(svc)
    if sid == 0x31 and proto != 2 and _const_at(svc, 1) in (1, 2, 3) and _const_at(svc, 2) is not None:
        return _const_at(svc, 1) == 1                      # UDS routine control: 31 01 <id> = start
    if sid in (0x2E, 0x22, 0x2F, 0x31, 0x3B, 0x30): return _const_at(svc, 1) is not None
    return sid not in (0x32, 0x33)

def inputs_of(pk, svc):
    """(name, dop) of the variable request inputs, in the order the app's request builder consumes them"""
    out = []
    def walk(di, name, depth=0):
        if di < 0 or di >= len(pk.dops) or depth > 8: return
        d = pk.dops[di]
        if d[0] == "t":
            for f in d[1]:
                if f[4] is None and f[3] >= 0: walk(f[3], f[0], depth + 1)
        elif d[0] == "s": out.append((pk.src.get(name, pk.strs[name]) if 0 <= name < len(pk.strs) else "", d))
    for p in svc[4]:
        if p[4] is None and p[5] >= 0: walk(p[5], p[0])
    return out

NOT_AN_OPTION = re.compile(r"(?i)not.?(defined|used|available)|nicht (definiert|verwendet)|ung(ü|ue)ltig|invalid|^unknown$|^-+$")

def _options(pk, dop):
    return [(sc[0], pk.src.get(sc[5], pk.strs[sc[5]]) if 0 <= sc[5] < len(pk.strs) else "") for sc in dop[6] if isinstance(sc[0], (int, float))] if dop[5] == 3 else []

def _preset(pk, st, name, dop):
    for pat, val in st.get("preset", []):
        if not re.search(pat, name): continue
        if val == "$only":
            opts = [lo for lo, t in _options(pk, dop) if t and not NOT_AN_OPTION.search(t)]
            return opts[0] if len(set(opts)) == 1 else None
        if isinstance(val, tuple) and val[0] == "$opt":
            opts = [lo for lo, t in _options(pk, dop) if re.search(val[1], t)]
            return opts[0] if len(set(opts)) == 1 else None
        return val
    return None

def find_step(mods, st, pk):
    mre = re.compile(st["mod"]); fre = re.compile(st["fn"]); nre = re.compile(st["no"]) if st.get("no") else None
    for mi, m in enumerate(mods):
        if not m["ids"] or not mre.search(m["k"]): continue
        for si, svc in enumerate(m["svc"]):
            if si >= len(m["_src"]): break
            if st.get("sid") and _sid(svc) not in st["sid"]: continue
            if not _addressable(svc, m["p"]): continue
            name = "%s %s" % m["_src"][si]
            if not fre.search(name) or (nre and nre.search(name)): continue
            ins = inputs_of(pk, svc)
            if st.get("need") and not any(re.search(st["need"], n) for n, d in ins): continue
            vals = [_preset(pk, st, n, d) for n, d in ins]             # None = the user chooses
            if st.get("max_ask") is not None and sum(1 for v in vals if v is None) > st["max_ask"]: continue
            return mi, si, vals
    return None

def resolve(mods, pk, progress=print):
    out = []
    for j in JOBS:
        steps = []; ok = True
        for st in j["steps"]:
            hit = find_step(mods, st, pk)
            if hit is None:
                if st.get("opt"): continue
                ok = False; break
            steps.append([hit[0], hit[1], 1 if any(v is None for v in hit[2]) else 0, hit[2], st.get("label", "")])
        if ok and steps:
            out.append({"id": j["id"], "t": j["title"], "d": j["desc"], "w": j.get("warn", ""), "steps": steps})
            progress("  job %-28s %s" % (j["id"], "; ".join("%s #%d %s" % (mods[s[0]]["k"], s[1], s[3]) for s in steps)))
    return out
