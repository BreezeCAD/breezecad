"""The basic path of Сдвиг, Лофт, Пружина, Резьба: the sketches are made by commands, the operation by the ribbon dialog;
the result must equal the same operation made by command (and the known formula where there is one)."""
import math

from _features import *

V_SWEEP = math.pi * 9 * (30 + math.pi / 2 * 10 + 20)      # a D6 pipe along a path of known length (Pappus)
V_COIL = math.pi * 1 * 4 * math.hypot(2 * math.pi * 10, 8)  # D2 wire, 4 turns of R10, pitch 8


def sweep_inputs(b):
    path = b.run("sketch.create", plane="XZ")["id"]
    b.run("sketch.add", sketch=path, items=[{"type": "line", "from": [0, 0], "to": [0, 30]},
                                            {"type": "arc", "center": [10, 30], "start": [10, 40], "end": [0, 30]},
                                            {"type": "line", "from": [10, 40], "to": [30, 40]}])
    prof = b.run("sketch.create", plane="XY")["id"]
    b.run("sketch.add", sketch=prof, items=[{"type": "circle", "center": [0, 0], "diameter": 6}])
    return prof, path


def sweep(new):
    b = new()
    new_part(b)
    prof, path = sweep_inputs(b)
    r = b.run("feature.sweep", sketch=prof, path=path)
    check(r["state"] == "ok", f"сдвиг командой: {r}")
    b.settle()
    want = props(b)
    check(close(want["volume_mm3"], V_SWEEP, 1e-6), f"сдвиг: объём {want['volume_mm3']}, по формуле {V_SWEEP}")
    b = new()
    new_part(b)
    sweep_inputs(b)
    b.click("feature.sweep")
    b.settle()
    b.click("done")
    b.settle()
    clean(b, "сдвиг, лента")
    same_part(props(b), want, "сдвиг: диалог и команда")


def loft_inputs(b):
    b.run("work.plane", plane="XY", offset=30)
    s1 = b.run("sketch.create", plane="XY")["id"]
    b.run("sketch.add", sketch=s1, items=[{"type": "rectangle", "center": [0, 0], "width": 20, "height": 20}])
    s2 = b.run("sketch.create", plane="Плоскость1")["id"]
    b.run("sketch.add", sketch=s2, items=[{"type": "circle", "center": [0, 0], "diameter": 20}])
    return s1, s2


def loft(new):
    b = new()
    new_part(b)
    s1, s2 = loft_inputs(b)
    r = b.run("feature.loft", sections=[s1, s2])
    check(r["state"] == "ok", f"лофт командой: {r}")
    b.settle()
    want = props(b)
    lo, hi = math.pi * 100 * 30, 20 * 20 * 30
    check(lo < want["volume_mm3"] < hi, f"лофт: объём {want['volume_mm3']} вне {lo}..{hi}")
    check(close(want["size_mm"][2], 30, 1e-9), "лофт: высота не 30")
    b = new()
    new_part(b)
    loft_inputs(b)
    b.click("feature.loft")
    b.settle()
    panel = b.panel()
    check("##done" in panel, f"лофт: нет кнопки «Готово»; панель {panel}")
    b.click("done")
    b.settle()
    clean(b, "лофт, лента")
    same_part(props(b), want, "лофт: диалог и команда")
    b.click("feature.loft")                 # «Прямые» переходы: the ruled loft has less volume than the smooth one here? only must build
    b.settle()
    b.click("ruled|Прямые")
    b.settle()
    b.click("done")
    b.settle()
    # Over the smooth loft the ruled one adds nothing (it lies inside it): that
    # is a warning now («Добавление ничего не изменило»), not an error — it
    # only must build.
    bad = [(f["name"], f.get("message")) for f in b.run("part.tree")["features"] if f.get("state") == "error"]
    check(not bad, f"лофт прямыми: операции с ошибкой: {bad}")


def coil(new):
    b = new()
    new_part(b)
    sk = b.run("sketch.create", plane="XZ")["id"]
    b.run("sketch.add", sketch=sk, items=[{"type": "centerline", "from": [0, 0], "to": [0, 40]},
                                          {"type": "circle", "center": [10, 5], "diameter": 2}])
    r = b.run("feature.coil", sketch=sk, pitch=8, turns=4)
    check(r["state"] == "ok", f"пружина командой: {r}")
    b.settle()
    want = props(b)
    check(abs(want["volume_mm3"] - V_COIL) / V_COIL < 0.02, f"пружина: объём {want['volume_mm3']}, оценка {V_COIL}")
    check(want["size_mm"][2] > 32, f"пружина: высота {want['size_mm'][2]}, ждали около 4 x 8 + 2")
    b = new()
    new_part(b)
    sk = b.run("sketch.create", plane="XZ")["id"]
    b.run("sketch.add", sketch=sk, items=[{"type": "centerline", "from": [0, 0], "to": [0, 40]},
                                          {"type": "circle", "center": [10, 5], "diameter": 2}])
    b.click("feature.coil")
    b.settle()
    panel = b.panel()
    check("##done" in panel, f"пружина: нет кнопки «Готово»; панель {panel}")
    b.fill("value", 8, nth=0)
    b.fill("value", 4, nth=1)
    b.click("done")
    b.settle()
    clean(b, "пружина, лента")
    same_part(props(b), want, "пружина: диалог и команда")


def thread(new):
    b = new()
    new_part(b)
    sk = b.run("sketch.create", plane="XY")["id"]
    b.run("sketch.add", sketch=sk, items=[{"type": "circle", "center": [0, 0], "diameter": 8}])
    b.run("feature.extrude", sketch=sk, distance=20)
    b.settle()
    plain = props(b)["volume_mm3"]
    r = b.run("feature.thread", axis_point=[0, 0, 0], axis_dir=[0, 0, 1], diameter=8)
    check(r["state"] == "ok", f"резьба командой: {r}")
    b.settle()
    want = props(b)
    check(want["volume_mm3"] < plain, f"резьба не сняла материал: {want['volume_mm3']} >= {plain}")
    check(want["volume_mm3"] > plain * 0.6, f"резьба сняла слишком много: {want['volume_mm3']} из {plain}")
    b = new()
    new_part(b)
    sk = b.run("sketch.create", plane="XY")["id"]
    b.run("sketch.add", sketch=sk, items=[{"type": "circle", "center": [0, 0], "diameter": 8}])
    b.run("feature.extrude", sketch=sk, distance=20)
    b.settle()
    b.click("feature.thread")
    b.settle()
    b.click_world(4, 0, 10)               # the cylinder
    b.settle()
    b.click("done")
    b.settle()
    clean(b, "резьба, лента")
    same_part(props(b), want, "резьба: диалог и команда")


def run(new):
    sweep(new)
    thread(new)
    loft(new)
    coil(new)
