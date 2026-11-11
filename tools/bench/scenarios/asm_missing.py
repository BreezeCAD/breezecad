"""Утраченный файл детали: сборка открывается с понятной ошибкой, остальные детали и
зависимости на месте; вернули файл — всё как было. Удаление компонента, на который
ссылаются зависимости: зависимости уходят вместе с ним, отмена и повтор."""
import math
import os
import shutil
import tempfile

from _asm import *


def run(new):
    folder = tempfile.mkdtemp(prefix="asm-missing-")
    files = unit_parts(new(), folder)
    b = new()
    unit_reference(b, files)
    want = comps(b)
    path = os.path.join(folder, "uzel.bzc")
    b.run("doc.save", path=path)
    b.run("doc.close", discard_changes=True)
    # удаление компонента с зависимостями (крышка: 3 зависимости с ней и 2 болта, вставленных в неё)
    b.run("app.open", path=path); b.settle()
    n_con = len(comps(b)["constraints"])
    b.run("assembly.remove", component=4); b.settle()
    c = comps(b)
    check(len(c["components"]) == 5 and all(4 not in k["components"] for k in c["constraints"]),
          "удаление крышки оставило зависимости на неё")
    b.run("doc.undo"); b.settle()
    check(len(comps(b)["components"]) == 6 and len(comps(b)["constraints"]) == n_con, "отмена удаления компонента")
    b.run("doc.redo"); b.settle()
    check(len(comps(b)["components"]) == 5, "повтор удаления компонента")
    b.run("doc.close", discard_changes=True)
    # файл детали утрачен
    os.rename(files["kryshka"], files["kryshka"] + ".off")
    b.run("app.open", path=path); b.settle()
    c = comps(b)
    lost = [x for x in c["components"] if x.get("problem")]
    check(len(lost) == 1 and lost[0]["name"].startswith("kryshka") and "не найден" in lost[0]["problem"], f"утраченный компонент: {lost}")
    check(len(c["components"]) == 6 and len(c["constraints"]) == n_con, "утрата файла потеряла компоненты или зависимости")
    for x in c["components"]:
        if x["id"] != 4:
            ref = next(y for y in want["components"] if y["id"] == x["id"])
            check(math.dist(x["placement"]["origin"], ref["placement"]["origin"]) < 1e-3, f"{x['name']} сдвинулся при утрате файла")
    check(any(n["kind"] == "error" for n in b.notes()), "нет сообщения об ошибке")
    b.run("doc.close", discard_changes=True)
    os.rename(files["kryshka"] + ".off", files["kryshka"])
    b.run("app.open", path=path); b.settle()
    same_places(comps(b), want, "после возврата файла")
    all_satisfied(b, "после возврата файла")
    check(not [x for x in comps(b)["components"] if x.get("problem")], "ошибка осталась после возврата файла")
