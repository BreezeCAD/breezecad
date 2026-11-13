"""Угол 30° между гранями двух деталей, стоящих параллельно (самый обычный
исходный случай: створка рядом с плитой). Ждём: деталь повернулась на 30°;
выходит: зависимость «не выполняется», деталь не двинулась (угол 0 — мёртвая
точка решателя: производная скалярного произведения нормалей равна нулю).
Если деталь перед этим довернуть хотя бы на 5° — решается (проверяется ниже)."""
import math
import tempfile

from _asm import *


def run(new):
    folder = tempfile.mkdtemp(prefix="asm-angle-")
    b = new()
    plate = make_plate(b, folder)
    cover = make_plate(b, folder, "kryshka", t=6)
    b = new()
    new_assembly(b)
    b.run("assembly.place", file=plate)
    b.run("assembly.place", file=cover, at=[100, 0, 0])
    b.settle()
    # control: a part turned by 5 degrees first goes to 30
    b.run("assembly.move", component=2, rotate={"axis": [1, 0, 0], "angle": 5})
    b.settle()
    r = constrain(b, "angle", plane_of(b, 1, [0, 0, 1]), plane_of(b, 2, comp(b, 2)["placement"]["z_axis"]), angle=30)
    check(r["state"] == "satisfied", f"контроль (старт 5°): состояние {r['state']}")
    # the case: parallel start
    b.run("doc.undo"); b.run("doc.undo")
    b.settle()
    b.run("assembly.move", component=2, to=[100, 0, 0])
    b.settle()
    z = comp(b, 2)["placement"]["z_axis"]
    check(abs(z[2] - 1) < 1e-6, f"подготовка: деталь не параллельна ({z})")
    r = constrain(b, "angle", plane_of(b, 1, [0, 0, 1]), plane_of(b, 2, [0, 0, 1]), angle=30)
    z = comp(b, 2)["placement"]["z_axis"]
    got = math.degrees(math.acos(max(-1, min(1, z[2]))))
    check(r["state"] == "satisfied" and abs(got - 30) < 1e-3, f"угол 30° от параллельного старта: состояние {r['state']}, угол {got:.2f}°")
