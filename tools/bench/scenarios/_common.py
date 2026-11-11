"""What several scenarios share: looking at the model the way a person does
(where is that line now?) and comparing a part built by hand with its reference."""
from __future__ import annotations

import math

from bench import Bench, BenchError, check, close


def last_sketch(b: Bench) -> dict:
    tree = b.run("part.tree")
    sketches = [f for f in tree["features"] if f["type"] == "sketch"]
    check(sketches, "нет эскиза в детали")
    return b.run("sketch.get", sketch=sketches[-1]["id"])


def mid(line: dict) -> tuple[float, float]:
    return (line["from"][0] + line["to"][0]) / 2, (line["from"][1] + line["to"][1]) / 2


def lines_by_side(sketch: dict) -> dict:
    """The sides of an axis-aligned rectangle: top, bottom, left, right."""
    ls = sketch["lines"]
    horizontal = [l for l in ls if abs(l["from"][1] - l["to"][1]) < 1e-6]
    vertical = [l for l in ls if abs(l["from"][0] - l["to"][0]) < 1e-6]
    check(len(horizontal) >= 2 and len(vertical) >= 2, "в эскизе нет прямоугольника")
    return {"top": max(horizontal, key=lambda l: l["from"][1]), "bottom": min(horizontal, key=lambda l: l["from"][1]),
            "left": min(vertical, key=lambda l: l["from"][0]), "right": max(vertical, key=lambda l: l["from"][0])}


def dimension(b: Bench, at: tuple[float, float], text_at: tuple[float, float], value):
    """The Размер tool is on: pick what is at `at`, put the text at `text_at`, type the value."""
    b.click_sketch(*at)
    b.click_sketch(*text_at)
    b.settle()
    check(b.state().get("text_input"), f"размер не встал: поле значения не открылось (щелчки {at} и {text_at})")
    b.enter(value)
    b.settle()


def props(b: Bench) -> dict:
    return b.run("part.properties")


def same_part(got: dict, want: dict, what: str, rel: float = 1e-6):
    """Volume, area and size: two ways to build one part must agree to the micron."""
    for key in ("volume_mm3", "area_mm2"):
        check(close(got[key], want[key], rel), f"{what}: {key} {got[key]:.6f}, а в эталоне {want[key]:.6f}")
    for i, axis in enumerate("XYZ"):
        check(close(got["size_mm"][i], want["size_mm"][i], rel, 1e-6),
              f"{what}: размер по {axis} {got['size_mm'][i]:.6f}, а в эталоне {want['size_mm'][i]:.6f}")
    check(got["bodies"] == want["bodies"], f"{what}: тел {got['bodies']}, а в эталоне {want['bodies']}")


def clean(b: Bench, what: str):
    """Nothing went wrong on the way: no error message on screen, every operation built."""
    errors = b.errors_on_screen()
    check(not errors, f"{what}: на экране ошибка: {errors}")
    bad = [f["name"] for f in b.run("part.tree")["features"] if f.get("state") not in ("ok", None)]
    check(not bad, f"{what}: операции с ошибкой: {bad}")


def new_part(b: Bench):
    b.click("home-part")
    b.settle()
    check(b.state().get("type") == "part", "новая деталь не открылась")


def start_sketch_on_xy(b: Bench):
    """Начать эскиз with the ribbon, the XY plane picked in the view."""
    b.click("sketch.start")
    b.settle()
    b.click_world(30, -30, 0)   # inside XY's rectangle, away from the other two planes
    b.settle()
    check(b.state().get("mode") == "sketch", "эскиз на XY не начался")


def finish_sketch(b: Bench):
    b.key("Escape")
    b.click("pinned-finish")
    b.settle()
    check(b.state().get("mode") == "model", "эскиз не закрылся")
