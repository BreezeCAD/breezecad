"""BUG: the draft (Уклон) of an extrusion is not checked: 90, 180 and 1e9 degrees are accepted without a message
(90 gives an empty piece, 180 acts as 0, 1e9 a huge odd body). Expected, as in Inventor: only -90 < angle < 90."""
from _features import *


def run(new):
    b = new()
    new_part(b)
    sk = b.run("sketch.create", plane="XY")["id"]
    b.run("sketch.add", sketch=sk, items=[{"type": "circle", "center": [0, 0], "diameter": 8}])
    bad = []
    for angle in (90, 180, 1e9, 95):
        r = b.try_run("feature.extrude", sketch=sk, distance=10, taper=angle)
        refused = "error" in r or r.get("state") == "error"
        if not refused:
            bad.append((angle, r.get("state"), round(r.get("volume_mm3", -1), 3)))
        b.settle()
        if not refused:
            b.run("doc.undo")
            b.settle()
    check(not bad, f"уклон вне (-90°; 90°) принят без ошибки: {bad}")
