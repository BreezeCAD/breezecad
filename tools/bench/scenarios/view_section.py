"""«Разрез вида»: срез заштрихован, как в Inventor, и обращён к смотрящему.

Плита с отверстием; разрез включается кнопкой ленты, плоскости XY/XZ/YZ,
пять масштабов. На срезе должны быть видны линии штриховки (сильный перепад
яркости вдоль строки пикселей), а не ровная заливка."""
import math

from _common import *
from imageutil import crop, read_ppm


def reveal(b, action):
    """The ribbon may be scrolled: its arrow brings the end into view."""
    for _ in range(6):
        if b.find(action):
            return
        b.click("##arrow")
        b.settle()


def contrast_at(b, x, y, z, name):
    """Brightness range along a row of pixels through a model point."""
    px, py = b.pixel(x, y, z)
    b.move(600, 860)   # the cursor off the part: no hover highlight
    b.settle()
    w, h, d = crop(read_ppm(b.shot(name)), (px - 60, py, px + 60, py + 1))
    row = [d[i] for i in range(0, len(d), 3)]
    return max(row) - min(row)


def run(new):
    b = new()
    new_part(b)
    sk = b.run("sketch.create", plane="XY")["id"]
    b.run("sketch.add", sketch=sk, items=[{"type": "rectangle", "corner": [-40, -25], "opposite": [40, 25]},
                                          {"type": "circle", "center": [0, 0], "diameter": 8}])
    b.run("feature.extrude", sketch=sk, distance=10)
    b.settle()
    reveal(b, "view.section")
    b.click("view.section")
    b.settle()

    # XY from the top: the cut at z = 5, the hatching on it at every zoom.
    b.click("seg:section-plane|XY")
    b.click("tab:Вид")
    b.click("view.top")
    b.settle()
    for step in (0, 3, 2, -3, -3):
        if step:
            b.scroll(900, 560, step)
            b.settle()
        c = contrast_at(b, -20, 10, 5, f"xy{step}")
        check(c > 40, f"срез XY без штриховки (перепад яркости {c}, масштаб {step})")

    # From the default view, each plane: the cut faces the viewer and is hatched.
    b.click("view.home")
    b.settle()
    for plane, point in (("XZ", (-20, 0, 6)), ("YZ", (0, 15, 6))):
        b.click(f"seg:section-plane|{plane}")
        b.settle()
        c = contrast_at(b, *point, f"iso-{plane}")
        check(c > 40, f"срез {plane} не виден или без штриховки (перепад {c})")
    check(not b.errors_on_screen(), f"ошибка на экране: {b.errors_on_screen()}")
