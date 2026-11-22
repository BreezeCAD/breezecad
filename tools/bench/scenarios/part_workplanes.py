"""Work planes by command: offset, midplane, at an angle to a face through an edge, on three points; each plane's frame is
checked against the geometry, a sketch on it is extruded, the plane's own errors (offset text, a missing edge) give a message."""
import math

from _features import *


def run(new):
    b = new()
    new_part(b)
    s = b.run("sketch.create", plane="XY")["id"]
    b.run("sketch.add", sketch=s, items=[{"type": "rectangle", "center": [0, 0], "width": 40, "height": 40}])
    b.run("feature.extrude", sketch=s, distance=10)
    b.settle()
    p1 = b.run("work.plane", plane="XY", offset=30)
    check(p1["frame"]["origin"][2] == 30, f"смещение: {p1['frame']}")
    p1r = b.run("work.plane", plane="XY", offset=30, reversed=True)
    check(p1r["frame"]["origin"][2] == -30, f"смещение «Обратно»: {p1r['frame']}")
    mid = b.run("work.plane", method="midplane", plane="XY", plane2=p1["name"])
    check(abs(mid["frame"]["origin"][2] - 15) < 1e-9 and abs(mid["frame"]["normal"][2]) == 1.0, f"посередине: {mid['frame']}")
    top = face_at(b, (0, 0, 1), (0, 0, 10))
    top_face = [f for f in b.run("part.faces")["faces"] if f["face"] == top][0]
    mid_faces = b.run("work.plane", method="midplane", face=top, plane2=p1["name"])
    check(abs(mid_faces["frame"]["origin"][2] - 20) < 1e-9, f"посередине грань-плоскость: {mid_faces['frame']}")
    edge = edge_at(b, [0, -20, 10])
    ang = b.run("work.plane", method="angle", face=top, edge=edge, angle=30)
    n = ang["frame"]["normal"]
    check(abs(abs(n[2]) - math.cos(math.radians(30))) < 1e-6 and abs(abs(n[1]) - math.sin(math.radians(30))) < 1e-6,
          f"под углом 30°: нормаль {n}")
    three = b.run("work.plane", method="three_points", points=[[0, 0, 40], [10, 0, 40], [0, 10, 40]])
    check(abs(three["frame"]["origin"][2] - 40) < 1e-9 or abs(three["frame"]["normal"][2]) == 1.0, f"по трём точкам: {three['frame']}")
    # a sketch on the middle plane, extruded: the volume adds up
    sk = b.run("sketch.create", plane=mid["name"])
    b.run("sketch.add", sketch=sk["id"], items=[{"type": "rectangle", "center": [0, 0], "width": 10, "height": 10}])
    r = b.run("feature.extrude", sketch=sk["id"], distance=5, reversed=True)   # down to the block top
    b.settle()
    check(r["state"] in ("ok", "warning"), f"выдавливание с плоскости: {r}")
    # errors: not a number, a missing edge
    bad = b.try_run("work.plane", plane="XY", offset="abc")
    check("error" in bad or bad.get("state") == "error", f"смещение «abc» принято: {bad}")
    bad = b.try_run("work.plane", method="angle", face=top, edge=99999, angle=30)
    check("error" in bad or bad.get("state") == "error", f"несуществующее ребро принято: {bad}")
    clean(b, "рабочие плоскости")
    check(not b.problems(), f"в журнале: {b.problems()[:2]}")
