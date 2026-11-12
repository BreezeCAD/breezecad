"""Правка на месте двойным щелчком, изменение размера (сборка перестраивается, зависимости держат),
деталь, созданная в сборке кнопкой «Создать» с проекцией рёбер соседа руками, изменение источника →
окно «Обнаружено изменение» (автоадаптация), сохранение и повторное открытие."""
import tempfile

from _asm import *
from _common import finish_sketch


def run(new):
    folder = tempfile.mkdtemp(prefix="asm-inplace-")
    files = unit_parts(new(), folder)
    b = new()
    unit_reference(b, files)
    # размер плиты: двойной щелчок, толщина 10 -> 15, возврат кнопкой
    open_in_place(b, "plita:1")
    b.run("part.set_parameter", name="d5", value=15); b.settle()
    click_wait(b, "##return-to-assembly"); b.settle()
    check(b.state().get("type") == "assembly", "кнопка «Вернуться» не вернула в сборку")
    check(abs(comp(b, 2)["placement"]["origin"][2] - 15) < 1e-3, "стойка не поднялась вслед за плитой")
    check(abs(comp(b, 4)["placement"]["origin"][2] - 45) < 1e-3, "крышка не поднялась")
    all_satisfied(b, "после изменения толщины плиты")
    no_clash(b, "после изменения толщины плиты")
    # другие правки плиты: скругление углов, второе отверстие — зависимости держат
    b.run("assembly.edit_in_place", component=1); b.settle()
    edges = [e["edge"] for e in b.run("part.edges")["edges"] if e.get("kind") == "line" and abs(e["direction"][2]) > 0.99
             and all(f["kind"] == "plane" for f in e["ref"]["faces"])]
    b.run("feature.fillet", edges=edges, radius=5); b.settle()
    b.run("assembly.return_to_assembly"); b.settle()
    all_satisfied(b, "после скругления плиты")
    # деталь в сборке, руками: «Создать», имя, грань крышки
    look(b, UP)
    b.click("assembly.create"); b.settle()
    b.click("##new-part-name"); b.key("ctrl+a"); b.type("gasket"); b.settle()
    pick(b, -30, 10, 51)   # верх крышки: 45 + 6
    check(b.state().get("mode") == "sketch", f"эскиз на грани крышки не начался: {b.brief()}")
    b.click("sketch.project"); b.settle()
    b.move_sketch(40, 10); b.settle()
    b.click_sketch(40, 10); b.settle()
    sk = next(f["id"] for f in b.run("part.tree")["features"] if f["type"] == "sketch")
    lines = len(b.run("sketch.get", sketch=sk)["lines"])
    check(lines >= 4, f"проекция ребра крышки дала {lines} линий")
    finish_sketch(b)
    b.click("feature.extrude"); b.settle()
    b.run("view.set", fit=True); b.settle(); b.click_world(20, 10, 0); b.settle()
    b.fill("Расстояние", 2); b.click("done"); b.settle()
    click_wait(b, "##return-to-assembly"); b.settle()
    check(b.state().get("type") == "assembly", "не вернулись в сборку")
    gb = gasket_box(b)
    check(abs(gb["max"][0] - 40) < 0.2, f"деталь по проекции: габарит {gb}")
    # источник изменился: крышка 80 -> 100 по X
    open_in_place(b, "kryshka:1")
    b.run("part.set_parameter", name="d1", value=100); b.settle()
    click_wait(b, "##return-to-assembly"); b.settle()
    click_wait(b, "##change-dialog")
    choose_variant(b, 1)   # автоадаптация
    check(abs(gasket_box(b)["max"][0] - 50) < 0.2, f"автоадаптация: {gasket_box(b)}")
    all_satisfied(b, "после автоадаптации")
    # сохранить и открыть заново: те же положения
    want = comps(b)
    path = tempfile.mkdtemp(prefix="asm-inplace-save-") + "/uzel.bzc"
    b.run("doc.save", path=path)
    b.run("doc.close", discard_changes=True)
    b.run("app.open", path=path); b.settle()
    same_places(comps(b), want, "после открытия")
    all_satisfied(b, "после открытия")
    check(abs(gasket_box(b)["max"][0] - 50) < 0.2, "после открытия прокладка не той ширины")
    check(not b.problems(), f"в журнале сбой {b.problems()[:2]}")
