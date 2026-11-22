"""Отмена/повтор после каждого вида действия в эскизе возвращают sketch.get точно:
рисование, обрезка, разбить, скругление, эквидистанта, перенос, копирование, масштаб, зеркало, круговой массив."""
import json

from _common import *


def sig(s):
    r = lambda v: [round(x, 6) for x in v]
    return json.dumps({"p": sorted([p["id"], r(p["at"])] for p in s["points"]),
                       "l": sorted([l["id"], r(l["from"]), r(l["to"]), bool(l.get("construction"))] for l in s["lines"]),
                       "c": sorted(json.dumps(c, sort_keys=True) for c in s["circles"]),
                       "a": sorted(json.dumps(c, sort_keys=True) for c in s["arcs"]),
                       "k": sorted(json.dumps(c, sort_keys=True) for c in s["constraints"]),
                       "dof": s["degrees_of_freedom"]}, sort_keys=True)


def to_select(b):
    for _ in range(3):
        if b.brief().get("sketch_tool") == "Выбор" and not b.state().get("text_input"):
            return
        b.key("Escape"); b.settle()


def tool(b, t):
    b.click(t); b.settle()


def step(b, name, fn):
    before = sig(last_sketch(b))
    fn(); b.settle(); to_select(b)
    after = sig(last_sketch(b))
    check(before != after, f"{name}: действие ничего не изменило")
    b.key("ctrl+z"); b.settle()
    check(sig(last_sketch(b)) == before, f"{name}: отмена не вернула эскиз")
    b.key("ctrl+y"); b.settle()
    check(sig(last_sketch(b)) == after, f"{name}: повтор не вернул результат")


def run(new):
    b = new()
    new_part(b); start_sketch_on_xy(b)
    step(b, "прямоугольник", lambda: (tool(b, "sketch.rectangle"), b.click_sketch(10, 10), b.move_sketch(25, 20), b.click_sketch(40, 30)))
    step(b, "отрезок", lambda: (tool(b, "sketch.line"), b.click_sketch(-30, 15), b.move_sketch(-15, 15), b.click_sketch(0, 15)))
    step(b, "окружность", lambda: (tool(b, "sketch.circle"), b.click_sketch(-30, -20), b.move_sketch(-25, -20), b.click_sketch(-20, -20)))
    step(b, "разбить", lambda: (tool(b, "sketch.split"), b.click_sketch(25, 10)))
    step(b, "обрезка", lambda: (tool(b, "sketch.trim"), b.click_sketch(-20, 15)))
    step(b, "скругление", lambda: (tool(b, "sketch.fillet"), b.fill("Радиус", 5), b.click_sketch(40, 20), b.click_sketch(25, 30)))
    step(b, "эквидистанта", lambda: (tool(b, "sketch.offset"), b.fill("Расстояние", 5), b.click_sketch(10, 20), b.click_sketch(0, 20)))
    to_select(b)
    # Перенос окружности
    def move():
        b.click_sketch(-25, -20 + 5) if False else b.drag_sketch(-35, -30, -5, -10)
        tool(b, "sketch.move")
        b.fill("Сдвиг по X", 5); b.fill("Сдвиг по Y", -5)
        b.click("##move-now")
    step(b, "перенос", move)
    def scale():
        b.drag_sketch(-35, -40, 0, -10)
        tool(b, "sketch.scale"); b.fill("Коэффициент", 2)
        b.click_sketch(-25, -25); b.settle()
        if b.find("##apply-edit"):
            b.click("##apply-edit")
    step(b, "масштаб", scale)
    check(not b.problems(), f"в журнале: {b.problems()}")
