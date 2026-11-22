"""БАГ: поворот на 90° (и круговой массив с шагом 90°) схлопывает отрезки с горизонталью/вертикалью в точки.
Проверяем длины сторон после поворота ручным инструментом, командой sketch.edit и круговым массивом на 4."""
import math

from _common import *


def lens(s):
    return sorted(round(math.dist(l["from"], l["to"]), 6) for l in s["lines"] if not l.get("construction"))


def rect(b):
    b.click("sketch.rectangle")
    b.click_sketch(10, 10)
    b.move_sketch(25, 20)
    b.click_sketch(40, 30)
    b.settle()
    b.key("Escape")
    b.settle()


def run(new):
    # 1. Инструмент «Поворот», 90 градусов.
    b = new()
    new_part(b); start_sketch_on_xy(b)
    rect(b)
    check(lens(last_sketch(b)) == [20, 20, 30, 30], f"исходный прямоугольник: {lens(last_sketch(b))}")
    b.drag_sketch(5, 5, 50, 40)
    b.settle()
    b.click("sketch.rotate")
    b.settle()
    b.fill("Угол", 90)
    b.click_sketch(10, 10)
    b.settle()
    got = lens(last_sketch(b))
    check(got == [20, 20, 30, 30], f"поворот на 90°: стороны {got}, ждали [20, 20, 30, 30] (схлопнулись?)")
    # 2. Круговой массив на 4 копии: 90- и 270-градусные копии.
    b = new()
    new_part(b); start_sketch_on_xy(b)
    b.click("sketch.rectangle")
    b.click_sketch(10, 10); b.move_sketch(15, 12); b.click_sketch(20, 15)
    b.settle(); b.key("Escape"); b.settle()
    b.drag_sketch(5, 5, 25, 20)
    b.settle()
    b.click("sketch.patternCircular"); b.settle()
    b.fill("Количество", 4)
    b.move_sketch(0, 0); b.click_sketch(0, 0)
    b.click("##apply-edit"); b.settle()
    got = lens(last_sketch(b))
    check(got == [5] * 8 + [10] * 8, f"круговой массив на 4: стороны {got}, ждали 4 прямоугольника 10x5")
