"""An L-bracket: base 60 x 40 x 5, a back flange 60 x 20 x 30 (two extrusions), a plain hole, a counterbored hole,
a countersunk hole, both bend fillets and the three kinds of chamfer.
Reference by commands, then by hand two ways (hole tool and ribbon; circles cut and chamfered, hot keys)."""
import math

from _features import *

T = math.tan(math.radians(30))
V_BASE = 12000 + 60 * 20 * 25                       # 42000: plate + flange - the overlap
V_PLAIN = math.pi * 9 * 5                            # D6 through 5
V_CBORE = math.pi * (6.25 * 2 + 25 * 3)              # D5 through 5, D10 x 3 deep
V_SINK = math.pi * 6.25 * 20 + math.pi * 2 / 3 * (4.5 ** 2 + 4.5 * 2.5 + 2.5 ** 2) - math.pi * 6.25 * 2   # D5 through 20, 90 deg D9
V_FILLET_IN = (1 - math.pi / 4) * 9 * 60
V_FILLET_OUT = (1 - math.pi / 4) * 64 * 60
V_CH_EQUAL = 2 * 2 / 2 * 60
V_CH_TWO = 2 * 1 / 2 * 60
V_CH_ANGLE = 4 * 4 * T / 2 * 60
V_ALL = V_BASE - V_PLAIN - V_CBORE - V_SINK + V_FILLET_IN - V_FILLET_OUT - V_CH_EQUAL - V_CH_TWO - V_CH_ANGLE


def base_by_commands(b):
    new_part(b)
    s1 = b.run("sketch.create", plane="XY")["id"]
    b.run("sketch.add", sketch=s1, items=[{"type": "rectangle", "corner": [0, 0], "opposite": [60, 40]}])
    b.run("feature.extrude", sketch=s1, distance=5)
    s2 = b.run("sketch.create", plane="XY")["id"]
    b.run("sketch.add", sketch=s2, items=[{"type": "rectangle", "corner": [0, 20], "opposite": [60, 40]}])
    b.run("feature.extrude", sketch=s2, distance=30)
    b.settle()


def reference(b):
    base_by_commands(b)
    p = props(b)
    check(close(p["volume_mm3"], V_BASE, 1e-9), f"эталон: объём заготовки {p['volume_mm3']}, по формуле {V_BASE}")
    top = face_at(b, (0, 0, 1), (15, 10, 5))
    b.run("feature.hole", face=top, centers=[[15, 10, 5]], diameter=6, through_all=True)
    b.run("feature.hole", face=top, centers=[[45, 10, 5]], diameter=5, type="counterbore", head_diameter=10, head_depth=3,
          through_all=True)
    inner = face_at(b, (0, -1, 0), (30, 20, 20))
    b.run("feature.hole", face=inner, centers=[[30, 20, 20]], diameter=5, type="countersink", sink_diameter=9, through_all=True)
    b.settle()
    clean(b, "эталон, отверстия")
    p = props(b)
    want = V_BASE - V_PLAIN - V_CBORE - V_SINK
    check(close(p["volume_mm3"], want, 1e-7), f"эталон: объём с отверстиями {p['volume_mm3']}, по формуле {want}")
    b.run("feature.fillet", edges=[edge_at(b, [30, 20, 5])], radius=3)
    b.run("feature.fillet", edges=[edge_at(b, [30, 40, 0])], radius=8)
    b.run("feature.chamfer", edges=[edge_at(b, [30, 40, 30])], distance=2)
    b.run("feature.chamfer", edges=[edge_at(b, [30, 20, 30])], mode="two_distances", distance=2, distance2=1)
    b.run("feature.chamfer", edges=[edge_at(b, [30, 0, 5])], mode="distance_angle", distance=4, angle=30)
    b.settle()
    clean(b, "эталон")
    p = props(b)
    check(close(p["volume_mm3"], V_ALL, 1e-7), f"эталон: объём {p['volume_mm3']}, по формуле {V_ALL}")
    # «distance_angle»: the distance goes along the reference (upper) face, the angle is between it and the chamfer
    want_n = (0, -math.sin(math.radians(60)) / 1, math.cos(math.radians(60)))
    found = [f for f in b.run("part.faces")["faces"] if f["kind"] == "plane" and abs(f["normal"][0]) < 1e-6
             and abs(f["normal"][1] + 0.5) < 1e-5 and abs(f["normal"][2] - math.sqrt(3) / 2) < 1e-5]
    check(found, "фаска «С углом»: нет грани с наклоном 30° к опорной (верхней) грани")
    return p


def draw_base_clicks(b):
    new_part(b)
    start_sketch_on_xy(b)
    zoom_out(b)
    rect_clicks(b, (0, 0), (60, 40))
    finish_clicked(b)
    extrude_ribbon(b, 5)
    b.click("sketch.start")
    b.settle()
    b.click_world(30, -30, 0)
    b.settle()
    zoom_out(b)
    rect_clicks(b, (0, 20), (60, 40))
    finish_clicked(b)
    extrude_ribbon(b, 30)
    clean(b, "заготовка, лента")
    check(close(props(b)["volume_mm3"], V_BASE, 1e-6), f"заготовка рукой: объём {props(b)['volume_mm3']}, ждали {V_BASE}")


def hole_tool(b, world, xy, diameter, kind, extra=()):
    """The Отверстие tool: the face by a click, the centre typed, «Применить» keeps the dialog for the next one."""
    b.settle()
    b.click_world(*world)
    b.settle()
    b.fill("##x", xy[0])
    b.fill("##y", xy[1])
    if kind == "cbore":
        b.click("type|Цековка")
    elif kind == "sink":
        b.click("type|Зенковка")
    b.fill("value", diameter, nth=0)
    for i, v in enumerate(extra, 1):
        b.fill("value", v, nth=i)
    for _ in range(4):   # the panel of a counterbore is longer than the window: a person scrolls it
        if b.find("depth-kind|Насквозь", exact=False):
            break
        b.scroll(200, 600, -5)
    b.click("depth-kind|Насквозь", exact=False)


def edge_ops(b, confirm):
    """Both bend fillets and the three chamfers; `confirm` presses Готово (the button or Enter)."""
    b.click("feature.fillet")
    b.settle()
    b.click_world(30, 20, 5)
    b.fill("Радиус", 3)
    confirm()
    b.settle()
    b.click("feature.chamfer")
    b.settle()
    b.click_world(30, 40, 30)
    b.fill("Расстояние", 2)
    confirm()
    b.settle()
    b.click("feature.chamfer")
    b.settle()
    b.click_world(30, 20, 30)
    b.click("chamfer-mode|Два размера")
    b.fill("Расстояние 1", 2) if b.find("Расстояние 1") else b.fill("value", 2, nth=0)
    b.fill("value", 1, nth=1)
    confirm()
    b.settle()
    b.click("feature.chamfer")
    b.settle()
    b.click_world(30, 0, 5)
    b.click("chamfer-mode|С углом")
    b.fill("value", 4, nth=0)
    b.fill("value", 30, nth=1)
    confirm()
    b.settle()
    # the outer bend is on the back: turn the view
    b.click("feature.fillet")
    b.settle()
    turn_view(b)
    b.click_world(30, 40, 0)
    b.fill("Радиус", 8)
    confirm()
    b.settle()


def by_ribbon(b):
    draw_base_clicks(b)
    b.click("feature.hole")
    b.settle()
    hole_tool(b, (20, 5, 5), (15, 10), 6, "plain")
    b.click("apply")
    b.settle()
    hole_tool(b, (50, 5, 5), (45, 10), 5, "cbore", extra=(10, 3))
    b.click("apply")
    b.settle()
    hole_tool(b, (20, 20, 12), (30, 20), 5, "sink", extra=(9,))
    b.click("done")
    b.settle()
    clean(b, "лента, отверстия")
    want = V_BASE - V_PLAIN - V_CBORE - V_SINK
    got = props(b)["volume_mm3"]
    check(close(got, want, 1e-6), f"отверстия рукой: объём {got}, по формуле {want}")
    # fillet: the inner bend; «Отмена» leaves nothing
    n = len(tree_names(b))
    b.click("feature.fillet")
    b.settle()
    b.click_world(30, 20, 5)
    b.fill("Радиус", 3)
    b.settle()
    b.click("cancel")
    b.settle()
    check(len(tree_names(b)) == n and close(props(b)["volume_mm3"], got, 1e-9), "«Отмена» сопряжения оставила след")
    edge_ops(b, lambda: b.click("done"))
    clean(b, "лента")
    return props(b)


def circle_cut(b, pick, world, diameter, *, depth=None):
    """The face under `pick` by a click, S, a circle of the typed diameter centred at `world`, E: cut through or `depth` deep."""
    b.click_world(*pick)
    b.key("s")
    b.settle()
    check(b.state().get("mode") == "sketch", "эскиз на грани не начался")
    zoom_out(b, 1)
    b.key("c")
    b.fill("Диаметр", diameter)
    b.move_sketch(0, 0) if False else b.move(*b.pixel(*world))
    b.click_world(*world)
    b.settle()
    b.key("Escape", "Escape", "Escape")
    b.settle()
    check(b.state().get("mode") == "model", "Esc не вывел из эскиза")
    b.key("e")
    b.settle()
    b.click("Вырезать из тела")
    if depth is None:
        b.click("depth|Насквозь", exact=False)
    else:
        b.fill("Расстояние", depth)
    b.key("Return")
    b.settle()


def by_keys(b):
    new_part(b)
    b.key("s")
    b.settle()
    b.click_world(30, -30, 0)
    b.settle()
    zoom_out(b)
    b.key("r")
    b.click("Прямоугольник от центра")
    b.fill("Ширина", 60)
    b.fill("Высота", 40)
    b.move_sketch(35, 25)
    b.click_sketch(30, 20)
    b.move_sketch(40, 30)
    b.click_sketch(45, 30)
    b.settle()
    b.key("Escape", "Escape", "Escape")
    b.settle()
    check(b.state().get("mode") == "model", "Esc не вывел из эскиза")
    b.key("e")
    b.settle()
    b.enter(5)
    b.settle()
    b.key("s")
    b.settle()
    b.click_world(30, -30, 0)
    b.settle()
    zoom_out(b)
    b.key("r")
    b.click("Прямоугольник от центра")
    b.fill("Ширина", 60)
    b.fill("Высота", 20)
    b.move_sketch(35, 33)
    b.click_sketch(30, 30)
    b.move_sketch(40, 35)
    b.click_sketch(45, 35)
    b.settle()
    b.key("Escape", "Escape", "Escape")
    b.settle()
    b.key("e")
    b.settle()
    b.enter(30)
    b.settle()
    clean(b, "клавиши, заготовка")
    check(close(props(b)["volume_mm3"], V_BASE, 1e-6), f"заготовка клавишами: объём {props(b)['volume_mm3']}, ждали {V_BASE}")
    circle_cut(b, (25, 5, 5), (15, 10, 5), 6)
    # the counterbore: the wide circle first (a wider circle over a through hole is bug_part_cut_over_hole), then the narrow one
    circle_cut(b, (25, 5, 5), (45, 10, 5), 10, depth=3)
    circle_cut(b, (25, 5, 5), (45, 10, 5), 5)
    circle_cut(b, (30, 20, 25), (30, 20, 20), 5)
    # the countersink: the edge of the hole chamfered 2 x 2 is a 90 degree cone of D9
    b.click("feature.chamfer")
    b.settle()
    b.click_world(32.5, 20, 20)
    b.fill("Расстояние", 2)
    b.key("Return")
    b.settle()
    clean(b, "клавиши, отверстия")
    want = V_BASE - V_PLAIN - V_CBORE - V_SINK
    got = props(b)["volume_mm3"]
    check(close(got, want, 1e-6), f"отверстия клавишами: объём {got}, по формуле {want}")
    edge_ops(b, lambda: b.key("Return"))
    clean(b, "клавиши")
    return props(b)


def run(new):
    want = reference(new())
    same_part(by_ribbon(new()), want, "кронштейн, лента и инструмент «Отверстие»")
    same_part(by_keys(new()), want, "кронштейн, клавиши, круги и фаска")
