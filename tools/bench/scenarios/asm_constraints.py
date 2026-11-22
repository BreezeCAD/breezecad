"""Виды зависимостей и их правка: смещение, переворот, угол, касание, симметрия,
соединения; правка в панели свойств, подавление и удаление из контекстного меню
дерева, Del; отмена и повтор после каждого вида действия; противоречие и избыточность."""
import tempfile

from _asm import *


def sat(r, what):
    check(r["state"] == "satisfied", f"{what}: состояние {r['state']} ({r})")


def origin_z(b, cid):
    return comp(b, cid)["placement"]["origin"][2]


def run(new):
    folder = tempfile.mkdtemp(prefix="asm-con-")
    b = new()
    plate = make_plate(b, folder)
    cover = make_plate(b, folder, "kryshka", t=6)
    post = make_tube(b, folder, inner=9)
    b = new()
    new_assembly(b)
    b.run("assembly.place", file=plate)
    b.run("assembly.place", file=cover, at=[100, 0, 0])
    b.settle()
    # Совмещение со смещением
    top, bottom = plane_of(b, 1, [0, 0, 1]), plane_of(b, 2, [0, 0, -1])
    r = constrain(b, "mate", top, bottom, offset=5)
    sat(r, "совмещение со смещением 5")
    check(abs(origin_z(b, 2) - 15) < 1e-3, f"зазор 5: крышка на z={origin_z(b, 2)}, ждали 15")
    mate_id = comps(b)["constraints"][-1]["id"]

    # Правка зависимости в панели «Свойства» (как руками): выбрать в дереве, открыть вкладку, ввести значение
    b.click("##rail-tree"); b.settle()
    b.click("tree:Совмещение:1"); b.settle()
    b.click("##rail-props"); b.settle()
    b.click("##con-value"); b.key("ctrl+a"); b.type("12"); b.key("Return"); b.settle()
    check(abs(origin_z(b, 2) - 22) < 1e-3, f"смещение 12 в панели: крышка на z={origin_z(b, 2)}, ждали 22")
    b.click("seg:con-flip|Да"); b.settle()
    p = comp(b, 2)["placement"]
    check(p["z_axis"][2] < -0.99 and abs(p["origin"][2] - 22) < 1e-3, f"заподлицо: {p}")
    # отмена по одному: переворот, смещение
    b.run("doc.undo"); b.settle()
    check(comp(b, 2)["placement"]["z_axis"][2] > 0.99, "отмена переворота не вернула крышку")
    b.run("doc.undo"); b.settle()
    check(abs(origin_z(b, 2) - 15) < 1e-3, f"отмена смещения: z={origin_z(b, 2)}, ждали 15")
    b.run("doc.redo"); b.run("doc.redo"); b.settle()
    check(abs(origin_z(b, 2) - 22) < 1e-3 and comp(b, 2)["placement"]["z_axis"][2] < -0.99, "повтор смещения и переворота")

    # Подавление из контекстного меню дерева, отмена, повтор
    b.click("##rail-tree"); b.settle()
    tree_menu(b, "Совмещение:1", 0)
    check(comps(b)["constraints"][0]["suppressed"], "подавление: зависимость не подавлена")
    check(comp(b, 2)["freedom"]["translations"] == 3, f"подавление не вернуло свободы: {comp(b, 2)['freedom']}")
    b.run("doc.undo"); b.settle()
    check(not comps(b)["constraints"][0]["suppressed"], "отмена подавления")
    b.run("doc.redo"); b.settle()
    check(comps(b)["constraints"][0]["suppressed"], "повтор подавления")
    tree_menu(b, "Совмещение:1", 0)   # включить
    check(not comps(b)["constraints"][0]["suppressed"], "включение")
    # Удаление Del
    b.click("tree:Совмещение:1"); b.key("Delete"); b.settle()
    check(not comps(b)["constraints"], "Del не удалил зависимость")
    b.run("doc.undo"); b.settle()
    check(len(comps(b)["constraints"]) == 1, "отмена удаления")
    b.run("doc.redo"); b.settle()
    check(not comps(b)["constraints"], "повтор удаления")
    b.run("doc.undo"); b.settle()

    # Противоречие: то же совмещение со смещением 30 — принято, но помечено, сообщение при этом есть
    r = constrain(b, "mate", top, bottom, offset=30)
    check(r["state"] == "failed" and "hint" in r, f"противоречивое совмещение: {r['state']}")
    b.run("doc.undo"); b.settle()
    # Избыточность
    r = constrain(b, "mate", top, bottom, offset=12)
    check(r["state"] == "redundant", f"избыточное: {r['state']}")
    b.run("doc.undo"); b.settle()
    all_satisfied(b, "после противоречия и избыточности")

    # Угол из НЕ параллельного положения (из параллельного — bug_asm_angle_parallel)
    b.run("assembly.remove", constraint=mate_id); b.settle()
    b.run("assembly.move", component=2, to=[100, 0, 0], rotate={"axis": [1, 0, 0], "angle": 10})
    b.settle()
    r = constrain(b, "angle", plane_of(b, 1, [0, 0, 1]), plane_of(b, 2, comp(b, 2)["placement"]["z_axis"]), angle=30)
    sat(r, "угол 30°")
    ang = math.degrees(math.acos(comp(b, 2)["placement"]["z_axis"][2]))
    check(abs(ang - 30) < 1e-3, f"угол вышел {ang}")
    aid = comps(b)["constraints"][-1]["id"]
    r = b.run("assembly.edit_constraint", id=aid, angle=60); b.settle()
    ang = math.degrees(math.acos(comp(b, 2)["placement"]["z_axis"][2]))
    check(abs(ang - 60) < 1e-3, f"угол после правки {ang}")
    b.run("doc.undo"); b.settle()
    ang = math.degrees(math.acos(comp(b, 2)["placement"]["z_axis"][2]))
    check(abs(ang - 30) < 1e-3, f"отмена правки угла: {ang}")
    b.run("assembly.remove", constraint=aid); b.settle()

    # Касание: стойка на боку на плите
    b.run("assembly.place", file=post, at=[0, 100, 0], rotate={"axis": [1, 0, 0], "angle": 90}); b.settle()
    pid = comps(b)["components"][-1]["id"]
    cyl = {"component": pid, "face": b.run("assembly.faces", component=pid, kind="cylinder", limit=0)["faces"][0]["face"]}
    r = constrain(b, "tangent", plane_of(b, 1, [0, 0, 1]), cyl)
    sat(r, "касание")
    check(abs(origin_z(b, pid) - 10 - 10) < 0.6 or abs(comp(b, pid)["box"]["min"][2] - 10) < 0.01,
          f"касание: низ стойки {comp(b, pid)['box']['min'][2]}")
    # Симметрия, с переворотом (из параллельного положения без flipped нужен поворот на 180° — см. bug_asm_symmetry_flip)
    b.run("assembly.remove", component=pid); b.settle()
    b.run("assembly.move", component=2, to=[100, 0, 0], rotate={"axis": [1, 0, 0], "angle": -10}); b.settle()
    r = constrain(b, "symmetry", plane_of(b, 1, [0, 0, 1]), plane_of(b, 2, comp(b, 2)["placement"]["z_axis"]), plane={"origin": "xy"}, flipped=True)
    sat(r, "симметрия (flipped)")
    check(not b.problems(), f"в журнале сбой {b.problems()[:2]}")
