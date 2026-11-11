"""A stepped shaft by revolution: 360 degrees, a part of a turn, and a groove cut by revolution.
Reference by commands, then by hand two ways (ribbon + mouse; hot keys + typed length/angle)."""
import math

from _common import *

PROFILE = [(0, 0), (0, 10), (30, 10), (30, 15), (60, 15), (60, 0)]   # sketch on XZ: u = X, v = Z
V_FULL = math.pi * (100 * 30 + 225 * 30)                              # r10 x 30 and r15 x 30
V_GROOVE = 2 * math.pi * 13.5 * (5 * 3)                               # 5 x 3 ring cut at r 12..15


def reference(b, extent="full", angle=None):
    new_part(b)
    sk = b.run("sketch.create", plane="XZ")["id"]
    b.run("sketch.add", sketch=sk, items=[{"type": "centerline", "from": [0, 0], "to": [60, 0]},
                                          {"type": "polyline", "points": [list(p) for p in PROFILE], "closed": True}])
    kw = {} if extent == "full" else {"extent": extent, "angle": angle}
    b.run("feature.revolve", sketch=sk, **kw)
    b.settle()
    clean(b, "эталон")
    return props(b)


def reference_groove(b):
    p = reference(b)
    check(close(p["volume_mm3"], V_FULL, 1e-9), f"вал: объём {p['volume_mm3']}, по формуле {V_FULL}")
    sk = b.run("sketch.create", plane="XZ")["id"]
    b.run("sketch.add", sketch=sk, items=[{"type": "centerline", "from": [0, 0], "to": [60, 0]},
                                          {"type": "rectangle", "corner": [40, 12], "opposite": [45, 16]}])
    b.run("feature.revolve", sketch=sk, operation="cut")
    b.settle()
    clean(b, "эталон с канавкой")
    p = props(b)
    check(close(p["volume_mm3"], V_FULL - V_GROOVE, 1e-9), f"канавка: объём {p['volume_mm3']}, по формуле {V_FULL - V_GROOVE}")
    return p


def open_xz_sketch(b, key):
    if key:
        b.key("s")
    else:
        b.click("sketch.start")
    b.settle()
    b.click_world(30, 0, 30)   # inside the XZ rectangle
    b.settle()
    check(b.state().get("mode") == "sketch", "эскиз на XZ не начался")


def draw_ribbon(b, profile=True):
    """Centre line and the closed outline by clicks on the grid (the ribbon's Отрезок tool)."""
    b.click("sketch.line")
    b.click("Осевая")
    b.move_sketch(5, 5)
    b.click_sketch(0, 0)
    b.move_sketch(30, 0)
    b.click_sketch(60, 0)
    b.settle()
    b.key("Escape")
    if profile:
        b.click("sketch.line")
        b.click("Основная")
        for p in PROFILE:
            b.move_sketch(p[0] + 1, p[1] + 1)
            b.click_sketch(*p)
        b.settle()
        b.key("Escape")
    b.settle()


def draw_typed(b):
    """The same by hot key L and typed length and angle (positive angles only)."""
    b.key("l")
    b.click("Осевая")
    b.move_sketch(5, 5)
    b.click_sketch(0, 0)
    b.fill("Длина", 60, confirm=None)
    b.fill("Угол", 0, confirm=None)
    b.key("Return")
    b.settle()
    b.key("Escape")
    b.click("Основная")
    b.move_sketch(2, 2)
    b.click_sketch(0, 0)
    for length, angle in ((10, 90), (30, 0), (5, 90), (30, 0), (15, 270)):
        b.fill("Длина", length, confirm=None)
        b.fill("Угол", angle, confirm=None)
        b.key("Return")
        b.settle()
    b.key("Escape")
    b.settle()


def finish(b):
    b.click("pinned-finish")
    b.settle()
    check(b.state().get("mode") == "model", "эскиз не закрылся")


def by_ribbon(b, groove=True):
    new_part(b)
    open_xz_sketch(b, key=False)
    draw_ribbon(b)
    finish(b)
    b.click("feature.revolve")
    b.settle()
    b.click("done")
    b.settle()
    clean(b, "лента")
    if groove:
        open_xz_sketch(b, key=False)
        draw_ribbon(b, profile=False)
        b.click("sketch.rectangle")
        b.move_sketch(41, 13)
        b.click_sketch(40, 12)
        b.move_sketch(44, 15)
        b.click_sketch(45, 16)
        b.settle()
        b.key("Escape")
        finish(b)
        b.click("feature.revolve")
        b.settle()
        b.click("Вырезать из тела")
        b.click("done")
        b.settle()
    clean(b, "лента, канавка")
    return props(b)


def by_keys(b, groove=True):
    new_part(b)
    open_xz_sketch(b, key=True)
    draw_typed(b)
    b.key("Escape", "Escape")
    b.settle()
    b.key("r")
    b.settle()
    b.key("Return")
    b.settle()
    clean(b, "клавиши")
    if groove:
        open_xz_sketch(b, key=True)
        draw_ribbon(b, profile=False)
        b.key("r")
        b.click("Прямоугольник от центра")   # centre 42.5, 14; width 5, height 4
        b.fill("Ширина", 5)
        b.fill("Высота", 4)
        b.move_sketch(41, 13)
        b.click_sketch(42.5, 14)
        b.settle()
        b.key("Escape", "Escape", "Escape")
        b.settle()
        check(b.state().get("mode") == "model", "Esc не вывел из эскиза")
        b.key("r")
        b.settle()
        b.click("Вырезать из тела")
        b.key("Return")
        b.settle()
    clean(b, "клавиши, канавка")
    return props(b)


def partial_turn(new):
    """270 degrees: commands, then the Угол field; the profile lies on one side of the axis, so the volume is the share."""
    want = reference(new(), "angle", 270)
    check(close(want["volume_mm3"], V_FULL * 0.75, 1e-9), f"270°: объём {want['volume_mm3']}, а 3/4 полного {V_FULL * .75}")
    b = new()
    new_part(b)
    sk = b.run("sketch.create", plane="XZ")["id"]
    b.run("sketch.add", sketch=sk, items=[{"type": "centerline", "from": [0, 0], "to": [60, 0]},
                                          {"type": "polyline", "points": [list(p) for p in PROFILE], "closed": True}])
    b.click("feature.revolve")
    b.settle()
    b.click("extent|Угол", exact=False)
    b.fill("value", 270)
    b.click("apply")   # «Применить» keeps the dialog for a next operation; «Отмена» then leaves only the applied one
    b.settle()
    b.click("cancel")
    b.settle()
    check(b.state().get("operation") is None, "после «Применить» и «Отмена» операция осталась открытой")
    names = [f["name"] for f in b.run("part.tree")["features"] if f["type"] == "revolve"]
    check(names == ["Вращение1"], f"после «Применить» и «Отмена» операций вращения: {names}")
    same_part(props(b), want, "вращение на 270°, поле Угол")
    sym = reference(new(), "symmetric", 270)
    check(close(sym["volume_mm3"], want["volume_mm3"], 1e-9), "симметричное вращение на 270°: объём другой")
    check(not b.problems(), "в журнале программы ошибки")


def run(new):
    want = reference_groove(new())
    same_part(by_ribbon(new()), want, "вал, лента и мышь")
    same_part(by_keys(new()), want, "вал, клавиши и поля")
    partial_turn(new)
