"""A housing 80 x 60 x 30 centred on the origin: a shell 3 mm with the top face open, a plain hole in the floor
repeated by a rectangular array (2 x 2), a second hole repeated round the Z axis (4), a round lug on the +X wall
mirrored to the -X wall. Reference by commands; by hand: the Отверстие tool + array/mirror dialogs, then circle cuts
by hot keys with the arrays made of those cuts."""
import math

from _features import *
from part_bracket import circle_cut

V_SHELL = 80 * 60 * 30 - 74 * 54 * 27
V_A = 4 * math.pi * 9 * 3        # D6 through the 3 mm floor, 2 x 2
V_B = 4 * math.pi * 4 * 3        # D4, 4 round the axis
V_LUG = 2 * math.pi * 25 * 6     # D10 x 6 on each side wall
V_ALL = V_SHELL - V_A - V_B + V_LUG


def box_shell(b):
    new_part(b)
    sk = b.run("sketch.create", plane="XY")["id"]
    b.run("sketch.add", sketch=sk, items=[{"type": "rectangle", "center": [0, 0], "width": 80, "height": 60}])
    b.run("feature.extrude", sketch=sk, distance=30)
    b.run("feature.shell", thickness=3, open_faces=[face_at(b, (0, 0, 1), (0, 0, 30))])
    b.settle()
    check(close(props(b)["volume_mm3"], V_SHELL, 1e-9), f"оболочка: объём {props(b)['volume_mm3']}, по формуле {V_SHELL}")


def reference(b):
    box_shell(b)
    floor = face_at(b, (0, 0, 1), (0, 0, 3))
    a = b.run("feature.hole", face=floor, centers=[[30, 15, 3]], diameter=6, depth=3)["id"]
    b.run("feature.pattern", sources=[a], kind="rectangular", direction1="X", count1=2, spacing1=60, reversed1=True,
          direction2="Y", count2=2, spacing2=30, reversed2=True)
    floor = face_at(b, (0, 0, 1), (0, 0, 3))
    c = b.run("feature.hole", face=floor, centers=[[20, 0, 3]], diameter=4, depth=3)["id"]
    b.run("feature.pattern", sources=[c], kind="circular", axis="Z", count=4)
    wall = face_at(b, (1, 0, 0), (40, 0, 15))
    sk = b.run("sketch.create", face=wall)
    o, x, y = sk["plane"]["origin_in_model"], sk["plane"]["sketch_x_in_model"], sk["plane"]["sketch_y_in_model"]
    d = [40 - o[0], 0 - o[1], 15 - o[2]]
    u = sum(d[i] * x[i] for i in range(3))
    v = sum(d[i] * y[i] for i in range(3))
    b.run("sketch.add", sketch=sk["id"], items=[{"type": "circle", "center": [u, v], "diameter": 10}])
    lug = b.run("feature.extrude", sketch=sk["id"], distance=6)["id"]
    b.run("feature.mirror", sources=[lug], plane="YZ")
    b.settle()
    clean(b, "эталон")
    p = props(b)
    check(close(p["volume_mm3"], V_ALL, 1e-9), f"эталон: объём {p['volume_mm3']}, по формуле {V_ALL}")
    check(close(p["size_mm"][0], 92, 1e-9), f"эталон: ширина с ушками {p['size_mm'][0]}, ждали 92")
    return p


def source_last(b):
    """The operation just made is ticked in «Что копировать» by itself (as in Inventor the last one is taken); nothing to click."""
    check(b.find("##check", nth=-1), "в списке «Что копировать» нет операций")


def scroll_panel(b, label, **kw):
    for _ in range(4):
        if b.find(label, **kw):
            return
        b.scroll(200, 600, -5)


def pattern_rect(b, confirm):
    b.click("feature.patternRect")
    b.settle()
    source_last(b)
    b.click("seg:rev1|Обратно")
    b.fill("value", 2, nth=0)
    b.fill("value", 60, nth=1)
    b.click("seg:dir2|Y")
    b.settle()
    b.click("seg:rev2|Обратно")
    scroll_panel(b, "value", nth=3)
    b.fill("value", 2, nth=2)
    b.fill("value", 30, nth=3)
    confirm()
    b.settle()


def pattern_circular(b, confirm):
    b.click("feature.patternCircular")
    b.settle()
    source_last(b)
    b.click("seg:axis|Z")
    b.fill("value", 4, nth=0)
    confirm()
    b.settle()


def by_tools(b):
    new_part(b)
    start_sketch_on_xy(b)
    zoom_out(b)
    b.click("sketch.rectangle")
    b.click("Прямоугольник от центра")
    b.fill("Ширина", 80)
    b.fill("Высота", 60)
    b.move_sketch(5, 5)
    b.click_sketch(0, 0)
    b.move_sketch(20, 20)
    b.click_sketch(40, 30)
    b.settle()
    b.key("Escape")
    finish_clicked(b)
    extrude_ribbon(b, 30)
    b.click("feature.shell")
    b.settle()
    b.click_world(0, 0, 30)             # the top face goes away
    b.fill("Толщина стенки", 3)
    b.click("done")
    b.settle()
    clean(b, "оболочка рукой")
    check(close(props(b)["volume_mm3"], V_SHELL, 1e-9), f"оболочка рукой: объём {props(b)['volume_mm3']}, ждали {V_SHELL}")
    b.click("feature.hole")
    b.settle()
    b.click_world(0, 10, 3)
    b.settle()
    b.fill("##x", 30)
    b.fill("##y", 15)
    b.fill("value", 6, nth=0)
    b.fill("value", 3, nth=1)
    b.click("done")
    b.settle()
    pattern_rect(b, lambda: b.click("done"))
    clean(b, "массив, лента")
    b.click("feature.hole")
    b.settle()
    b.click_world(0, 10, 3)
    b.settle()
    b.fill("##x", 20)
    b.fill("##y", 0)
    b.fill("value", 4, nth=0)
    b.fill("value", 3, nth=1)
    b.click("done")
    b.settle()
    pattern_circular(b, lambda: b.click("done"))
    clean(b, "круговой массив, лента")
    b.click_world(40, 5, 15)            # the +X wall outside: a sketch on it
    b.click("sketch.start")
    b.settle()
    check(b.state().get("mode") == "sketch", "эскиз на стенке не начался")
    zoom_out(b, 1)
    b.click("sketch.circle")
    b.fill("Диаметр", 10)
    b.click_world(40, 0, 15)
    b.settle()
    b.key("Escape")
    finish_clicked(b)
    extrude_ribbon(b, 6)
    b.click("feature.mirror")
    b.settle()
    source_last(b)
    b.click("seg:plane|YZ")
    b.click("done")
    b.settle()
    clean(b, "лента")
    return props(b)


def by_keys(b):
    new_part(b)
    b.key("s")
    b.settle()
    b.click_world(30, -30, 0)
    b.settle()
    zoom_out(b)
    b.key("r")
    b.click("Прямоугольник от центра")
    b.fill("Ширина", 80)
    b.fill("Высота", 60)
    b.move_sketch(5, 5)
    b.click_sketch(0, 0)
    b.move_sketch(20, 20)
    b.click_sketch(40, 30)
    b.settle()
    b.key("Escape", "Escape", "Escape")
    b.settle()
    b.key("e")
    b.settle()
    b.enter(30)
    b.settle()
    b.click("feature.shell")
    b.settle()
    b.click_world(0, 0, 30)
    b.fill("Толщина стенки", 3)
    b.key("Return")
    b.settle()
    clean(b, "оболочка, клавиши")
    # holes as circles cut 3 deep from the floor; the arrays copy those cuts
    circle_cut(b, (0, 10, 3), (30, 15, 3), 6, depth=3)
    pattern_rect(b, lambda: b.key("Return"))
    circle_cut(b, (0, 10, 3), (20, 0, 3), 4, depth=3)
    pattern_circular(b, lambda: b.key("Return"))
    clean(b, "массивы, клавиши")
    b.click_world(40, 5, 15)
    b.key("s")
    b.settle()
    check(b.state().get("mode") == "sketch", "эскиз на стенке не начался")
    zoom_out(b, 1)
    b.key("c")
    b.fill("Диаметр", 10)
    b.click_world(40, 0, 15)
    b.settle()
    b.key("Escape", "Escape", "Escape")
    b.settle()
    b.key("e")
    b.settle()
    b.enter(6)
    b.settle()
    b.click("feature.mirror")
    b.settle()
    b.click("seg:plane|YZ")
    b.key("Return")
    b.settle()
    clean(b, "клавиши")
    return props(b)


def run(new):
    want = reference(new())
    same_part(by_tools(new()), want, "корпус, лента и диалоги")
    same_part(by_keys(new()), want, "корпус, клавиши, круги-вырезы и массивы вырезов")
