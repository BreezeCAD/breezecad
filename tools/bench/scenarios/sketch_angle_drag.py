"""Угловой размер между двумя отрезками (два отрезка, текст между ними — как в Inventor),
затем перетаскивание конца недоопределённого отрезка: угол держится, длина меняется."""
from _common import *


def angle_of(l):
    return math.degrees(math.atan2(l["to"][1] - l["from"][1], l["to"][0] - l["from"][0]))


def run(new):
    b = new()
    new_part(b); start_sketch_on_xy(b)
    b.click("sketch.line")
    b.click_sketch(0, 0); b.move_sketch(15, 0); b.click_sketch(30, 0); b.settle()
    b.key("Escape"); b.settle()
    b.click("sketch.line")
    b.click_sketch(0, 0); b.move_sketch(10, 10); b.click_sketch(20, 20); b.settle()
    b.key("Escape"); b.settle()
    s = last_sketch(b)
    check(len(s["lines"]) == 2, f"ждали 2 отрезка, есть {len(s['lines'])}")
    b.click("sketch.dimension")
    b.click_sketch(25, 0); b.click_sketch(15, 15); b.click_sketch(18, 6); b.settle()
    check(b.state().get("text_input"), "угловой размер: поле значения не открылось")
    b.enter("30"); b.settle()
    s = last_sketch(b)
    angles = sorted(abs(angle_of(l)) % 180 for l in s["lines"])
    gap = abs(angles[1] - angles[0])
    check(abs(gap - 30) < 1e-6 or abs(gap - 150) < 1e-6, f"угол между отрезками {gap}, ждали 30")
    dims = [c for c in s["constraints"] if "parameter" in c]
    check(any(c["parameter"]["expression"] == "30" for c in dims), f"размер 30 не сохранился: {dims}")
    b.key("Escape"); b.settle()
    # Перетаскивание конца наклонного отрезка: угол 30° держится.
    slanted = max(s["lines"], key=lambda l: abs(angle_of(l)) % 180)
    end = slanted["to"] if math.hypot(*slanted["to"]) > 1 else slanted["from"]
    b.drag_sketch(end[0], end[1], end[0] + 10, end[1] + 2, hold=0.8, steps=12)
    b.settle()
    s2 = last_sketch(b)
    angles2 = sorted(abs(angle_of(l)) % 180 for l in s2["lines"])
    gap2 = abs(angles2[1] - angles2[0])
    check(abs(gap2 - 30) < 1e-4 or abs(gap2 - 150) < 1e-4, f"после перетаскивания угол {gap2}")
    moved = max(s2["lines"], key=lambda l: abs(angle_of(l)) % 180)
    check(abs(math.dist(moved["from"], moved["to"]) - math.dist(slanted["from"], slanted["to"])) > 1,
          "перетаскивание не изменило длину наклонного отрезка")
    # Одна отмена — назад к положению до перетаскивания.
    b.key("ctrl+z"); b.settle()
    s3 = last_sketch(b)
    back = max(s3["lines"], key=lambda l: abs(angle_of(l)) % 180)
    check(abs(math.dist(back["from"], back["to"]) - math.dist(slanted["from"], slanted["to"])) < 1e-6,
          "Ctrl+Z не вернул отрезок после перетаскивания")
    check(not b.problems(), f"в журнале: {b.problems()}")
