"""Деталь, созданная в сборке на грани, стоит на зависимостях (как в Inventor):
не закреплена, три «Совмещения» (XY с гранью, YZ и XZ с плоскостями начала
детали-хозяина), свобод нет; плита стала толще — деталь поднялась с её гранью."""
import tempfile

from _asm import *


def run(new):
    folder = tempfile.mkdtemp(prefix="asm-newpart-")
    b = new()
    plate = make_plate(b, folder)
    b = new()
    new_assembly(b)
    b.run("assembly.place", file=plate, at=[10, 5, 0])
    b.settle()
    b.run("assembly.create_part", name="kryshka", plane=plane_of(b, 1, (0, 0, 1)))
    b.settle()
    b.run("assembly.return_to_assembly")
    b.settle()
    c = comp(b, 2)
    check(not c["grounded"], "новая деталь закреплена, а не стоит на зависимостях")
    check(c.get("freedom") == {"rotations": 0, "translations": 0}, f"свободы новой детали: {c.get('freedom')}")
    all_satisfied(b, "после создания")
    placed(b, 2, [10, 5, 10], "новая деталь на верхней грани")
    b.run("assembly.edit_in_place", component=1)
    b.settle()
    name = next(p["name"] for p in b.run("part.parameters") if abs(p["value"] - 10) < 1e-9)
    b.run("part.set_parameter", name=name, value=15)
    b.settle()
    b.run("assembly.return_to_assembly")
    b.settle()
    placed(b, 2, [10, 5, 15], "новая деталь не поднялась с гранью плиты")
    all_satisfied(b, "после утолщения плиты")
