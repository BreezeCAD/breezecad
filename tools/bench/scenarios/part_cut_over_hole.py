"""(Was a bug, fixed by the lead) a cut by a circle drawn over an existing THROUGH hole (a counterbore made by hand) goes out of the part:
«Вырез не задел ни одного тела». The direction of a cut is chosen by one sample point of the profile
(Model::MaterialSide); that point lies in the hole, where no surface is met along the ray."""
import math

from _features import *


def run(new):
    b = new()
    new_part(b)
    sk = b.run("sketch.create", plane="XY")["id"]
    b.run("sketch.add", sketch=sk, items=[{"type": "rectangle", "corner": [0, 0], "opposite": [60, 40]}])
    b.run("feature.extrude", sketch=sk, distance=5)
    top = face_at(b, (0, 0, 1), (10, 10, 5))
    b.run("feature.hole", face=top, centers=[[30, 20, 5]], diameter=5, through_all=True)
    b.settle()
    v0 = props(b)["volume_mm3"]
    b.click_world(10, 10, 5)
    b.key("s")
    b.settle()
    check(b.state().get("mode") == "sketch", "эскиз на грани не начался")
    zoom_out(b, 1)
    b.key("c")
    b.fill("Диаметр", 10)
    b.click_world(30, 20, 5)
    b.settle()
    b.key("Escape", "Escape", "Escape")
    b.settle()
    b.key("e")
    b.settle()
    b.click("Вырезать из тела")
    b.fill("Расстояние", 3)
    b.key("Return")
    b.settle()
    clean(b, "вырез кругом над отверстием")
    want = v0 - (math.pi * 25 * 3 - math.pi * 6.25 * 3)
    got = props(b)["volume_mm3"]
    check(close(got, want, 1e-6), f"объём {got}, ждали {want}: вырез ушёл наружу")
