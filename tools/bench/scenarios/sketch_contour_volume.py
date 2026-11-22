"""Один контур (пластина 40x30, паз 20x6 по центру, выдавить 10) тремя способами: командами,
мышью с размерами, полями ширины/высоты; сравнение объёма."""
from _common import *


def reference(b):
    new_part(b)
    sk = b.run("sketch.create", plane="XY")["id"]
    b.run("sketch.add", sketch=sk, items=[{"type": "rectangle", "center": [0, 0], "width": 40, "height": 30},
                                            {"type": "slot", "from": [-5, 0], "to": [5, 0], "width": 6}])
    b.run("feature.extrude", sketch=sk, distance=10)
    b.settle()
    return props(b)


def extrude(b):
    b.key("Escape"); b.settle()
    if b.state().get("mode") == "sketch":
        finish_sketch(b)
    b.click("feature.extrude"); b.settle()
    b.click_world(14, 10, 0); b.settle()   # кольцо между краем и пазом
    b.fill("Расстояние", 10)
    b.click("done"); b.settle()
    clean(b, "выдавливание")
    return props(b)


def to_select(b):
    for _ in range(3):
        if b.brief().get("sketch_tool") == "Выбор" and not b.state().get("text_input"):
            return
        b.key("Escape"); b.settle()


def by_fields(b):
    new_part(b); start_sketch_on_xy(b)
    b.click("sketch.rectangle")
    b.click("Прямоугольник от центра")
    b.fill("Ширина", 40); b.fill("Высота", 30)
    b.move_sketch(10, 10); b.click_sketch(0, 0); b.move_sketch(20, 15); b.click_sketch(20, 15)
    b.settle()
    b.click("Паз: от центра до центра")
    b.fill("Ширина паза", 6)
    b.move_sketch(-5, 0); b.click_sketch(-5, 0); b.move_sketch(5, 0); b.click_sketch(5, 0)
    b.move_sketch(5, 3); b.click_sketch(5, 3)
    b.settle()
    to_select(b)
    return extrude(b)


def by_mouse(b):
    new_part(b); start_sketch_on_xy(b)
    b.click("sketch.rectangle")
    b.click_sketch(-20, -15); b.move_sketch(0, 0); b.click_sketch(20, 15)
    b.settle(); to_select(b)
    b.click("sketch.dimension")
    s = last_sketch(b); side = lines_by_side(s)
    x, y = mid(side["bottom"]); dimension(b, (x, y), (x, y - 8), 40)
    side = lines_by_side(last_sketch(b))
    x, y = mid(side["left"]); dimension(b, (x, y), (x - 9, y), 30)
    to_select(b)
    # центр прямоугольника — на начало координат диагональю: симметрия не нужна, сдвигаем явно
    p = last_sketch(b)
    side = lines_by_side(p)
    cx = (side["left"]["from"][0] + side["right"]["from"][0]) / 2
    cy = (side["top"]["from"][1] + side["bottom"]["from"][1]) / 2
    check(abs(cx) < 1e-6 and abs(cy) < 1e-6, f"прямоугольник не по центру: {cx},{cy}")
    b.click("sketch.rectangle")
    b.click("Паз: от центра до центра")
    b.fill("Ширина паза", 6)
    b.move_sketch(-5, 0); b.click_sketch(-5, 0); b.move_sketch(5, 0); b.click_sketch(5, 0)
    b.move_sketch(5, 3); b.click_sketch(5, 3)
    b.settle(); to_select(b)
    return extrude(b)


def run(new):
    want = reference(new())
    same_part(by_fields(new()), want, "поля ширины и высоты")
    same_part(by_mouse(new()), want, "мышь и размеры")
