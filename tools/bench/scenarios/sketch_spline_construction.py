"""Сплайн мышью: замкнутый (щелчок в первую точку) выдавливается; открытый — Enter.
Вспомогательная линия (G) через прямоугольник не делит контур: выдавливается один кусок."""
from _common import *


def run(new):
    b = new()
    new_part(b); start_sketch_on_xy(b)
    # Замкнутый сплайн из четырёх точек.
    b.click("sketch.spline")
    for u, v in ((10, 10), (40, 5), (45, 30), (15, 35)):
        b.click_sketch(u, v); b.settle()
    b.click_sketch(10, 10); b.settle()
    s = last_sketch(b)
    check(len(s.get("splines", [])) == 1, f"замкнутый сплайн не создан: {s.get('splines')}")
    check(s["splines"][0].get("closed"), f"сплайн не замкнут: {s['splines'][0]}")
    # Открытый сплайн, Enter.
    for u, v in ((-40, 0), (-30, 10), (-20, 0)):
        b.click_sketch(u, v); b.settle()
    b.key("Return"); b.settle()
    s = last_sketch(b)
    check(len(s.get("splines", [])) == 2, f"открытый сплайн не создан по Enter: {len(s.get('splines', []))}")
    finish_sketch(b)
    b.click("feature.extrude"); b.settle()
    b.fill("Расстояние", 5)
    b.click("done"); b.settle()
    clean(b, "сплайн")
    v = props(b)["volume_mm3"]
    check(v > 5 * 500, f"объём замкнутого сплайна {v} — слишком мал")

    # Прямоугольник со вспомогательной диагональю — один контур.
    b2 = new()
    new_part(b2); start_sketch_on_xy(b2)
    b2.click("sketch.rectangle")
    b2.click_sketch(0, 0); b2.move_sketch(20, 10); b2.click_sketch(40, 20); b2.settle()
    b2.key("Escape"); b2.settle()
    b2.click("sketch.line")
    b2.click_sketch(0, 0); b2.move_sketch(20, 10); b2.click_sketch(40, 20); b2.settle()
    b2.key("Escape"); b2.settle(); b2.key("Escape"); b2.settle()   # конец цепочки, затем «Выбор»
    check(b2.brief().get("sketch_tool") == "Выбор", f"после двух Esc инструмент {b2.brief().get('sketch_tool')}")
    s = last_sketch(b2)
    diag = [l for l in s["lines"] if abs(l["from"][0] - l["to"][0]) > 1e-6 and abs(l["from"][1] - l["to"][1]) > 1e-6]
    check(len(diag) == 1, "диагональ не нарисована")
    b2.click_sketch(20, 10); b2.settle()          # выделить диагональ
    b2.key("g"); b2.settle()                      # сделать вспомогательной
    s = last_sketch(b2)
    d = [l for l in s["lines"] if l["id"] == diag[0]["id"]][0]
    check(d.get("construction"), f"G не сделала линию вспомогательной: {d}")
    finish_sketch(b2)
    b2.click("feature.extrude"); b2.settle()
    b2.fill("Расстояние", 10)
    b2.click("done"); b2.settle()
    clean(b2, "прямоугольник со вспомогательной")
    v = props(b2)["volume_mm3"]
    check(abs(v - 40 * 20 * 10) < 1e-3, f"объём {v}, ждали 8000 (вспомогательная не должна делить контур)")
    check(not b2.problems(), f"в журнале: {b2.problems()}")
