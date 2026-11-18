#!/usr/bin/env python3
"""Vehicle lines OWIS builds data packs for.

id      : pack id / file name (data/<id>.json)
dir     : sub-directory below the PIWIS "Projects-V" folder
title   : shown in the vehicle list
vin     : VIN model types (VIN positions 7, 8 and 12) that select this pack automatically
sfd     : True for lines that use online authentication (SFD) for coding on at least part of the model years
pn      : first three characters of the control-unit part numbers of this line (fallback identification
          when no control unit gives a VIN)
"""
PLATFORMS = [
    dict(id="G1",    dir="G1",      title="Panamera (970) 2010-2016",                         vin=["970"],               pn=["970"]),
    dict(id="G2",    dir="G2",      title="Panamera (971) 2017-2023",                         vin=["971", "974"],        pn=["971", "9A7"], sfd=True),
    dict(id="9x1",   dir="9x1",     title="911 (991) / Boxster, Cayman (981, 718) 2012-2019", vin=["991", "981", "982"], pn=["991", "981", "982", "9P1"]),
    dict(id="CS1",   dir="CS1",     title="911 / Cayman Clubsport race cars (CS1)",           vin=[],                    pn=[]),
    dict(id="918S",  dir="918S",    title="918 Spyder 2013-2015",                             vin=["918"],               pn=["918"]),
    dict(id="992",   dir="992",     title="911 (992) 2019-",                                  vin=["992"],               pn=["992"], sfd=True),
    dict(id="Cajun", dir="Cajun",   title="Macan (95B) 2014-",                                vin=["95B"],               pn=["95B"]),
    dict(id="E2",    dir="E2",      title="Cayenne (92A) 2011-2017",                          vin=["92A"],               pn=["958", "7P5"]),
    dict(id="E3",    dir="E3",      title="Cayenne (9Y0) 2018-",                              vin=["9YA", "9YB", "9Y0"], pn=["9Y0", "9Y3"], sfd=True),
    dict(id="J1",    dir="J1",      title="Taycan (9J1) 2020-",                               vin=["Y1A", "Y1B", "Y1C"], pn=["9J1"], sfd=True),
    dict(id="9x7",   dir="MIG/9x7", title="911 (997) / Boxster, Cayman (987) 2005-2012",      vin=["997", "987"],        pn=["997", "987"]),
    dict(id="9x6",   dir="MIG/9x6", title="911 (996) / Boxster (986) 1997-2005",              vin=["996", "986"],        pn=["996", "986"]),
    dict(id="E1",    dir="MIG/E1",  title="Cayenne (9PA) 2003-2010",                          vin=["9PA"],               pn=["955", "957", "7L5"]),
]
BY_ID = {p["id"]: p for p in PLATFORMS}
