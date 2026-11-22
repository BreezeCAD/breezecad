"""Соединения: шарнир (крышка на плите), вал во втулке (болт в стойке: цилиндрическое),
жёсткое, шаровое; фиксация положения, перетаскивание мышью; отмена/повтор; сохранение и открытие."""
import tempfile

from _asm import *


def drag_at(b, pt, delta, steps=10):
    x, y = b.pixel(*pt)
    b.move(x, y)
    b.settle()
    b.drag(x, y, x + delta[0], y + delta[1], steps=steps)
    b.settle()


def run(new):
    folder = tempfile.mkdtemp(prefix="asm-joint-")
    b = new()
    files = unit_parts(b, folder)
    b = new()
    new_assembly(b)
    b.run("assembly.place", file=files["plita"])
    b.run("assembly.place", file=files["kryshka"], at=[100, 0, 0])
    b.settle()
    # шарнир по кромкам отверстий
    r = constrain(b, "rotational", E(b, 1, [25, 0, 10], 4.5), E(b, 2, [125, 0, 0], 4.5))
    check(r["state"] == "satisfied", f"шарнир: {r['state']}")
    check(comp(b, 2)["freedom"] == {"rotations": 1, "translations": 0}, f"свободы шарнира {comp(b, 2)['freedom']}")
    jid = comps(b)["constraints"][-1]["id"]
    b.run("assembly.edit_constraint", id=jid, locked=True, position=90); b.settle()
    p = comp(b, 2)["placement"]
    check(abs(p["x_axis"][1] - 1) < 1e-4, f"фиксация на 90°: x_axis {p['x_axis']}")
    b.run("doc.undo"); b.settle()
    check(abs(comp(b, 2)["placement"]["x_axis"][0] - 1) < 1e-4, "отмена фиксации шарнира")
    b.run("doc.redo"); b.settle()
    b.run("assembly.edit_constraint", id=jid, locked=False); b.settle()
    # мышью: крышка вращается вокруг оси шарнира, не уходя с неё
    look(b, (0.3, -1, 0.9))
    c0 = comp(b, 2)["placement"]["origin"]
    drag_at(b, (60, 0, 0), (0, 60))
    c1 = comp(b, 2)["placement"]["origin"]
    check(math.dist(c0[:2], [25, 0]) == math.dist(c1[:2], [25, 0]) or abs(math.dist(c1[:2], [25, 0]) - math.dist(c0[:2], [25, 0])) < 1e-3,
          f"шарнир: крышка ушла с оси ({c0} -> {c1})")
    check(abs(c1[2] - c0[2]) < 1e-3, "шарнир: крышка поднялась")
    all_satisfied(b, "шарнир после перетаскивания")

    # вал во втулке: цилиндрическое соединение болт-стойка
    b = new()
    new_assembly(b)
    b.run("assembly.place", file=files["stoyka"])
    b.run("assembly.place", file=files["bolt"], at=[100, 0, 40])
    b.settle()
    r = constrain(b, "cylindrical", E(b, 1, [0, 0, 30], 4.5), E(b, 2, [100, 0, 40], 7))
    check(r["state"] == "satisfied", f"вал во втулке: {r['state']}")
    check(comp(b, 2)["freedom"] == {"rotations": 1, "translations": 1}, f"свободы вала {comp(b, 2)['freedom']}")
    look(b, (0.3, -1, 0.5))
    z0 = comp(b, 2)["placement"]["origin"]
    drag_at(b, (0, 0, z0[2] + 3), (0, -50))
    z1 = comp(b, 2)["placement"]["origin"]
    check(math.dist(z0[:2], z1[:2]) < 1e-3, f"вал ушёл с оси втулки: {z0} -> {z1}")
    check(abs(z1[2] - z0[2]) > 1, "вал не скользит вдоль оси при перетаскивании")
    no_clash(b, "вал во втулке")
    # жёсткое и шаровое
    for kind, want in (("rigid", {"rotations": 0, "translations": 0}), ("ball", {"rotations": 3, "translations": 0})):
        b = new()
        new_assembly(b)
        b.run("assembly.place", file=files["plita"])
        b.run("assembly.place", file=files["kryshka"], at=[100, 0, 0])
        b.settle()
        r = constrain(b, kind, E(b, 1, [25, 0, 10], 4.5), E(b, 2, [125, 0, 0], 4.5))
        check(r["state"] == "satisfied" and comp(b, 2)["freedom"] == want, f"{kind}: {r['state']} {comp(b, 2)['freedom']}")
    # сохранение и открытие
    want = comps(b)
    path = tempfile.mkdtemp(prefix="asm-joint-save-") + "/j.bzc"
    b.run("doc.save", path=path); b.run("doc.close", discard_changes=True)
    b.run("app.open", path=path); b.settle()
    same_places(comps(b), want, "после открытия")
    got = [(c["type"], c["state"]) for c in comps(b)["constraints"]]
    check(got == [(c["type"], c["state"]) for c in want["constraints"]], f"зависимости после открытия: {got}")
    check(not b.problems(), f"в журнале сбой {b.problems()[:2]}")
