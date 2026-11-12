"""Лаги: сборка из 25 компонентов (плита с 24 отверстиями и 24 болта массивом):
время массива, перестройки при правке плиты на месте, зависимостей, отмены, сохранения, открытия, коллизий.
Пределы — с запасом для софтверного OpenGL; цифры печатаются."""
import tempfile
import time

from _asm import *

LIMITS = {"массив": 5, "коллизии": 5, "правка плиты на месте (вход)": 6, "изменение размера плиты": 6, "возврат": 5,
          "отмена": 5, "сохранение": 8, "открытие": 8}


def run(new):
    folder = tempfile.mkdtemp(prefix="asm-perf-")
    b = new()
    holes = [(x * 25 - 62.5, y * 25 - 37.5) for x in range(6) for y in range(4)]
    big = make_plate(b, folder, "big24", w=170, d=110, holes=holes, hole_d=9)
    bolt = make_bolt(b, folder)
    b = new()
    new_assembly(b)
    b.run("assembly.place", file=big)
    b.run("assembly.place", file=bolt, at=[200, 0, 30])
    b.settle()
    constrain(b, "insert", E(b, 1, [holes[0][0] + 4.5, holes[0][1], 10], 4.5), E(b, 2, [200, 0, 30], 4))
    times = {}

    def T(name, f):
        t = time.time()
        f()
        b.settle()
        times[name] = time.time() - t

    T("массив", lambda: b.run("assembly.pattern", component=2, kind="auto"))
    check(len(comps(b)["components"]) == 25, f"компонентов {len(comps(b)['components'])}, ждали 25")
    T("коллизии", lambda: b.run("assembly.clashes"))
    T("правка плиты на месте (вход)", lambda: b.run("assembly.edit_in_place", component=1))
    height = next(p["name"] for p in b.run("part.parameters") if p["owner"].startswith("Выдавливание"))
    T("изменение размера плиты", lambda: b.run("part.set_parameter", name=height, value=14))
    T("возврат", lambda: b.run("assembly.return_to_assembly"))
    check(abs(comp(b, 2)["placement"]["origin"][2] - 14) < 1e-3, "болты не поднялись за плитой")
    all_satisfied(b, "после правки плиты")
    T("отмена", lambda: b.run("doc.undo"))
    path = tempfile.mkdtemp(prefix="asm-perf-save-") + "/p.bzc"
    T("сохранение", lambda: b.run("doc.save", path=path))
    b.run("doc.close", discard_changes=True)
    T("открытие", lambda: b.run("app.open", path=path))
    check(len(comps(b)["components"]) == 25, "после открытия компонентов не 25")
    print("   замеры, с:", {k: round(v, 2) for k, v in times.items()})
    slow = {k: round(v, 1) for k, v in times.items() if v > LIMITS[k]}
    check(not slow, f"медленно (предел, с): {slow}")
