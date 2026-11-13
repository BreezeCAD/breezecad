"""«Проецировать» руками: эскиз на верхней грани, две кромки проецируются щелчком, прямоугольник от
спроецированного угла, вырез. Потом база меняется (d1, d2) — проекции и вырез идут за ней.
Эталон — формула объёма (неразмеренный прямоугольник тянется за углом, как в Inventor)."""
from _features import *


def run(new):
    b = new()
    new_part(b)
    sk = b.run("sketch.create", plane="XY")["id"]
    b.run("sketch.add", sketch=sk, items=[{"type": "rectangle", "corner": [0, 0], "opposite": [60, 40]}])
    b.run("feature.extrude", sketch=sk, distance=10)
    b.settle()
    b.click_world(30, 20, 10)
    b.key("s")
    b.settle()
    check(b.state().get("mode") == "sketch", "эскиз на грани не начался")
    b.click("sketch.project")
    b.settle()
    b.click_world(30, 0, 10)     # передняя верхняя кромка
    b.settle()
    b.click_world(60, 20, 10)    # правая верхняя кромка
    b.settle()
    s = last_sketch(b)
    check(len(s["lines"]) == 2, f"спроецировано отрезков {len(s['lines'])}, ждали 2")
    b.key("Escape")
    b.settle()
    b.key("r")
    b.click_sketch(60, 0)        # от спроецированного угла
    b.click_sketch(45, 15)
    b.settle()
    b.key("Escape")
    b.settle()
    finish_clicked(b)
    b.key("e")
    b.settle()
    b.click("Вырезать из тела")
    b.key("Return")
    b.settle()
    clean(b, "вырез от проекции")
    check(close(props(b)["volume_mm3"], 60 * 40 * 10 - 15 * 15 * 10, 1e-9), f"вырез от проекции: объём {props(b)['volume_mm3']}")
    b.run("part.set_parameter", name="d1", value=80)
    b.settle()
    clean(b, "после d1 = 80")
    # The projected corner went to x = 80; the rectangle, held only by it, stretched 45..80.
    check(close(props(b)["volume_mm3"], 80 * 40 * 10 - 35 * 15 * 10, 1e-9), f"после d1 = 80: объём {props(b)['volume_mm3']}")
    b.run("part.set_parameter", name="d2", value=25)
    b.settle()
    clean(b, "после d2 = 25")
    check(close(props(b)["volume_mm3"], 80 * 25 * 10 - 35 * 15 * 10, 1e-9), f"после d2 = 25: объём {props(b)['volume_mm3']}")
    b.key("ctrl+z")
    b.settle()
    check(close(props(b)["volume_mm3"], 80 * 40 * 10 - 35 * 15 * 10, 1e-9), f"отмена d2: объём {props(b)['volume_mm3']}")
