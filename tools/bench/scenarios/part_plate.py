"""A plate 80 x 50 x 10 with a through hole of 8 mm and four R5 corners:
by commands (the reference), then by hand two ways."""
import math

from _common import *


def reference(b):
    new_part(b)
    sk = b.run("sketch.create", plane="XY")["id"]
    b.run("sketch.add", sketch=sk, items=[{"type": "rectangle", "corner": [-40, -25], "opposite": [40, 25]}])
    b.run("feature.extrude", sketch=sk, distance=10)
    top = next(f["face"] for f in b.run("part.faces")["faces"] if f["kind"] == "plane" and f["normal"][2] > 0.9)
    b.run("feature.hole", face=top, centers=[[0, 0, 10]], diameter=8, through_all=True)
    b.settle()
    edges = [e["edge"] for e in b.run("part.edges")["edges"]
             if e.get("kind") == "line" and abs(e["direction"][2]) > 0.99 and abs(e["length"] - 10) < 1e-6
             and all(f["kind"] == "plane" for f in e["ref"]["faces"])]   # not the seam of the hole
    check(len(edges) == 4, f"эталон: вертикальных рёбер {len(edges)}, ждали 4")
    b.run("feature.fillet", edges=edges, radius=5)
    b.settle()
    clean(b, "эталон")
    p = props(b)
    want = 80 * 50 * 10 - math.pi * 16 * 10 - (4 - math.pi) * 25 * 10
    check(close(p["volume_mm3"], want, 1e-9), f"эталон: объём {p['volume_mm3']}, по формуле {want}")
    return p


def fillet_four_corners(b, lo, hi, radius, confirm):
    """The four vertical edges; the one behind the plate is reached by turning the view."""
    for x, y in ((lo[0], lo[1]), (hi[0], lo[1]), (hi[0], hi[1])):
        b.click_world(x, y, 5)
    x0, y0, x1, y1 = b.scene()
    b.drag((x0 + x1) / 2 + 200, y1 - 60, (x0 + x1) / 2 - 150, y1 - 60, button=3)   # the right button turns the view
    b.settle()
    b.click_world(lo[0], hi[1], 5)
    b.fill("Радиус", radius)
    confirm()
    b.settle()


def by_ribbon(b):
    """Way 1: everything from the ribbon, drawn roughly and brought to size with dimensions."""
    new_part(b)
    start_sketch_on_xy(b)
    b.click("sketch.rectangle")
    b.click_sketch(-37, -22)
    b.move_sketch(0, 0)
    b.click_sketch(41, 26)
    b.settle()
    b.key("Escape")
    b.click("sketch.dimension")
    side = lines_by_side(last_sketch(b))
    x, y = mid(side["top"])
    dimension(b, (x, y), (x, y + 8), 80)
    side = lines_by_side(last_sketch(b))
    x, y = mid(side["left"])
    dimension(b, (x, y), (x - 9, y), 50)
    finish_sketch(b)
    b.click("feature.extrude")
    b.settle()
    b.fill("Расстояние", 10)
    b.click("done")
    b.settle()
    p = props(b)
    lo, hi = p["bounds_mm"]["min"], p["bounds_mm"]["max"]
    cx, cy = (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2
    # The hole: clicked roughly, its centre then typed exactly.
    b.click("feature.hole")
    b.settle()
    b.click_world(cx + 3, cy + 2, 10)
    b.settle()
    b.fill("##x", round(cx, 6))
    b.fill("##y", round(cy, 6))
    b.fill("Диаметр", 8)
    b.click("Насквозь")
    b.click("done")
    b.settle()
    b.click("feature.fillet")
    b.settle()
    fillet_four_corners(b, lo, hi, 5, lambda: b.click("done"))
    clean(b, "лента")
    return props(b)


def by_keys(b):
    """Way 2: hot keys and typed sizes; the hole is a circle cut through, not the Отверстие tool."""
    new_part(b)
    b.key("s")
    b.settle()
    b.click_world(30, -30, 0)
    b.settle()
    check(b.state().get("mode") == "sketch", "клавиша S: эскиз не начался")
    b.key("r")
    b.click("Прямоугольник от центра")
    b.fill("Ширина", 80)
    b.fill("Высота", 50)
    b.move_sketch(10, 10)
    b.click_sketch(0, 0)
    b.move_sketch(20, 15)
    b.click_sketch(30, 20)
    b.settle()
    b.key("Escape", "Escape", "Escape")
    b.settle()
    check(b.state().get("mode") == "model", "Esc не вывел из эскиза")
    b.key("e")
    b.settle()
    b.enter(10)
    b.settle()
    # A sketch on the top face: the face first, then S.
    b.click_world(20, 10, 10)
    b.key("s")
    b.settle()
    check(b.state().get("mode") == "sketch", "эскиз на грани не начался")
    b.key("c")
    b.fill("Диаметр", 8)
    b.move_sketch(5, 5)
    b.click_sketch(0, 0)
    b.move_sketch(8, 3)
    b.click_sketch(4, 0)
    b.settle()
    b.key("Escape", "Escape", "Escape")
    b.settle()
    b.key("e")
    b.settle()
    b.click("Вырезать из тела")
    b.click("depth|Насквозь", exact=False)
    b.key("Return")
    b.settle()
    b.click("feature.fillet")   # F is «Показать всё» here, not Fillet
    b.settle()
    fillet_four_corners(b, [-40, -25, 0], [40, 25, 10], 5, lambda: b.key("Return"))
    clean(b, "клавиши")
    return props(b)


def run(new):
    want = reference(new())
    same_part(by_ribbon(new()), want, "плита, лента и мышь")
    same_part(by_keys(new()), want, "плита, клавиши и поля")
