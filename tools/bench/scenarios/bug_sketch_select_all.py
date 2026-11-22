"""Несоответствие Inventor: Ctrl+A в эскизе выделяет все элементы (здесь — ничего не происходит)."""
from _common import *


def run(new):
    b = new()
    new_part(b); start_sketch_on_xy(b)
    b.click("sketch.rectangle")
    b.click_sketch(10, 10); b.move_sketch(25, 20); b.click_sketch(40, 30)
    b.settle(); b.key("Escape"); b.settle()
    b.key("ctrl+a"); b.key("Delete"); b.settle()
    n = len(last_sketch(b)["lines"])
    check(n == 0, f"Ctrl+A, Delete: в эскизе осталось {n} отрезков (Ctrl+A не выделяет всё)")
