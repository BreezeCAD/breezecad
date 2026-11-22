"""Симметрия граней относительно плоскости XY: деталь надо перевернуть на 180°
(грань плиты +Z, у крышки тоже +Z, у зеркального отражения нормаль -Z).
Ждём: «выполнена»; выходит: «не выполняется», деталь перекошена (решатель
стартует из мёртвой точки: TurnOverBackwards пропускает Symmetry)."""
import tempfile

from _asm import *


def run(new):
    folder = tempfile.mkdtemp(prefix="asm-sym-")
    b = new()
    plate = make_plate(b, folder)
    cover = make_plate(b, folder, "kryshka", t=6)
    b = new()
    new_assembly(b)
    b.run("assembly.place", file=plate)
    b.run("assembly.place", file=cover, at=[100, 0, 0])
    b.settle()
    r = constrain(b, "symmetry", plane_of(b, 1, [0, 0, 1]), plane_of(b, 2, [0, 0, 1]), plane={"origin": "xy"})
    p = comp(b, 2)["placement"]
    check(r["state"] == "satisfied", f"симметрия: состояние {r['state']}, положение {p}")
    check(abs(p["origin"][2] + 4) < 1e-3 and abs(p["z_axis"][2] + 1) < 1e-4, f"крышка встала не зеркально: {p}")
