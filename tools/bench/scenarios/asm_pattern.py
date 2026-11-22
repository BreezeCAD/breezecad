"""Массив компонентов: по отверстиям (auto), прямоугольный, круговой, со стандартными
изделиями (болт + шайба); отмена/повтор, удаление массива, коллизии, замер на 49 штуках."""
import tempfile
import time

from _asm import *


def run(new):
    folder = tempfile.mkdtemp(prefix="asm-pat-")
    b = new()
    four = make_plate(b, folder, "plita4", holes=((-25, -15), (25, -15), (-25, 15), (25, 15)), hole_d=9)
    bolt = make_bolt(b, folder)
    holes = [(x * 20 - 60, y * 20 - 60) for x in range(7) for y in range(7)]
    big = make_plate(b, folder, "big49", w=160, d=160, holes=holes, hole_d=9)
    b = new()

    def base(plate):
        while b.run("app.documents"):
            b.run("doc.close", discard_changes=True)
        new_assembly(b)
        b.run("assembly.place", file=plate)
        b.run("assembly.place", file=bolt, at=[200, 0, 30])
        b.settle()

    base(four)
    constrain(b, "insert", E(b, 1, [25, 15, 10], 4.5), E(b, 2, [200, 0, 30], 4))
    res = b.run("assembly.pattern", component=2, kind="auto")
    b.settle()
    check(res["instances"] == 4, f"массив по отверстиям: {res}")
    check(len(comps(b)["components"]) == 5, "компонентов после массива не 5")
    all_satisfied(b, "массив по отверстиям")
    no_clash(b, "массив по отверстиям")
    b.run("doc.undo"); b.settle()
    check(len(comps(b)["components"]) == 2, "отмена массива")
    b.run("doc.redo"); b.settle()
    check(len(comps(b)["components"]) == 5, "повтор массива")
    b.run("assembly.remove", pattern=res["id"]); b.settle()
    check(len(comps(b)["components"]) == 2, "удаление массива оставило экземпляры")

    base(four)
    constrain(b, "insert", E(b, 1, [25, 15, 10], 4.5), E(b, 2, [200, 0, 30], 4))
    b.run("assembly.pattern", component=2, kind="rectangular", direction1=[-1, 0, 0], count1=2, spacing1=50,
          direction2=[0, -1, 0], count2=2, spacing2=30)
    b.settle()
    xs = sorted((round(c["placement"]["origin"][0]), round(c["placement"]["origin"][1])) for c in comps(b)["components"][1:])
    check(xs == [(-25, -15), (-25, 15), (25, -15), (25, 15)], f"прямоугольный массив: {xs}")
    no_clash(b, "прямоугольный массив")

    base(four)
    constrain(b, "insert", E(b, 1, [25, 15, 10], 4.5), E(b, 2, [200, 0, 30], 4))
    b.run("assembly.pattern", component=2, kind="circular", count=4, axis_origin=[0, 0, 0], axis_direction=[0, 0, 1])
    b.settle()
    r = b.run("assembly.clashes")["clashes"]
    check(r, "круговой массив вокруг начала: болты не в отверстиях, коллизии должны быть найдены")

    # стандартные изделия: болт ГОСТ 7798 с шайбой, массив тащит шайбу
    base(four)
    b.run("assembly.place", standard="gost7798", size="М8", length=30, at=[100, 0, 40])
    b.run("assembly.place", standard="gost11371", size="8", at=[100, 50, 10])
    b.settle()
    cs = comps(b)["components"]
    bolt_id, wash_id = cs[2]["id"], cs[3]["id"]
    constrain(b, "insert", E(b, 1, [25, 15, 10], 4.5), E(b, wash_id, [100, 50, 10], 4.2))
    re_ = b.run("assembly.faces", component=wash_id, limit=0)["round_edges"]
    top = next(e["edge"] for e in re_ if e["radius"] < 5 and e["axis"][2] > 0)
    constrain(b, "insert", {"component": wash_id, "edge": top}, E(b, bolt_id, [100, 0, 40], 4))
    b.run("assembly.remove", component=2); b.settle()
    r = b.run("assembly.pattern", component=bolt_id, kind="auto"); b.settle()
    check(r["instances"] == 4 and len(r["seed"]) == 2, f"массив болта с шайбой: {r}")
    check(len(comps(b)["components"]) == 1 + 8, f"болтов и шайб {len(comps(b)['components']) - 1}, ждали 8")
    all_satisfied(b, "стандартные изделия в массиве")
    no_clash(b, "стандартные изделия в массиве")

    # замер: 49 болтов
    base(big)
    constrain(b, "insert", E(b, 1, [-55.5, -60, 10], 4.5), E(b, 2, [200, 0, 30], 4))
    t = time.time()
    b.run("assembly.pattern", component=2, kind="auto"); b.settle()
    dt = time.time() - t
    check(len(comps(b)["components"]) == 50, "массив 49")
    check(dt < 6, f"массив из 49 за {dt:.1f} с (долго)")
    all_satisfied(b, "массив 49")
    check(not b.problems(), f"в журнале сбой {b.problems()[:2]}")
