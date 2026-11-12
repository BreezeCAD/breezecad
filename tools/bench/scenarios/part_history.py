"""History of a part: undo/redo give back exactly the same part.properties and part.tree (by commands and by Ctrl+Z / Ctrl+Y);
«Отмена» of every dialog leaves nothing; a double click on a tree row opens the operation and the rebuild follows;
an edit of a sketch dimension under an operation rebuilds what stands after it; suppression, deletion, the «Конец детали» marker."""
import math

from _features import *
from part_bracket import base_by_commands


def snap(b):
    p = props(b)
    t = b.run("part.tree")
    return (round(p["volume_mm3"], 6), round(p["area_mm2"], 6), tuple(round(x, 6) for x in p["size_mm"]), p["bodies"],
            tuple((f["id"], f["name"], f["type"], f["state"], f["suppressed"]) for f in t["features"]))


def same(a, b_, what):
    check(a == b_, f"{what}: было {a[:4]}, стало {b_[:4]}; дерево {[x[1:] for x in a[4] if x not in b_[4]]} / {[x[1:] for x in b_[4] if x not in a[4]]}")


def build(b, base_t=5, extra=True):
    """The L-bracket with a hole, an inner fillet and a chamfer; returns snapshots after every operation."""
    base_by_commands(b)
    steps = [snap(b)]
    top = face_at(b, (0, 0, 1), (15, 10, 5))
    b.run("feature.hole", face=top, centers=[[15, 10, 5]], diameter=6, through_all=True)
    b.settle()
    steps.append(snap(b))
    b.run("feature.fillet", edges=[edge_at(b, [30, 20, 5])], radius=3)
    b.settle()
    steps.append(snap(b))
    b.run("feature.chamfer", edges=[edge_at(b, [30, 40, 30])], distance=2)
    b.settle()
    steps.append(snap(b))
    return steps


def undo_redo(b, steps):
    for i in range(len(steps) - 1, 0, -1):
        b.run("doc.undo")
        b.settle()
        same(snap(b), steps[i - 1], f"отмена шага {i}")
    for i in range(1, len(steps)):
        b.run("doc.redo")
        b.settle()
        same(snap(b), steps[i], f"повтор шага {i}")
    # and the keys: Ctrl+Z three times, Ctrl+Y three times
    b.click_at(*b.center(b.wait_for("sketch.start")))   # focus the window on its ribbon, harmless
    b.key("Escape")
    for i in range(len(steps) - 1, 0, -1):
        b.key("ctrl+z")
        b.settle()
        same(snap(b), steps[i - 1], f"Ctrl+Z, шаг {i}")
    for i in range(1, len(steps)):
        b.key("ctrl+y")
        b.settle()
        same(snap(b), steps[i], f"Ctrl+Y, шаг {i}")


def cancel_leaves_nothing(b):
    """Every dialog: a preview, then «Отмена»: the tree and the properties are as before."""
    before = snap(b)
    b.click("feature.fillet")
    b.settle()
    b.click_world(30, 40, 0) if False else b.click_world(30, 20, 5)
    b.fill("Радиус", 1.5)
    b.settle()
    b.click("cancel")
    b.settle()
    same(snap(b), before, "«Отмена» сопряжения")
    b.click("feature.chamfer")
    b.settle()
    b.click_world(30, 0, 5)
    b.fill("Расстояние", 1)
    b.settle()
    b.click("cancel")
    b.settle()
    same(snap(b), before, "«Отмена» фаски")
    b.click("feature.hole")
    b.settle()
    b.click_world(45, 10, 5)
    b.settle()
    b.fill("value", 5, nth=0)
    b.settle()
    b.click("cancel")
    b.settle()
    same(snap(b), before, "«Отмена» отверстия")
    b.click("feature.shell")
    b.settle()
    b.click_world(30, 10, 5)
    b.fill("Толщина стенки", 1)
    b.settle()
    b.click("cancel")
    b.settle()
    same(snap(b), before, "«Отмена» оболочки")
    b.click("feature.patternRect")
    b.settle()
    b.fill("value", 3, nth=0)
    b.settle()
    b.click("cancel")
    b.settle()
    same(snap(b), before, "«Отмена» массива")
    b.click("feature.mirror")
    b.settle()
    b.click("seg:plane|YZ")
    b.settle()
    b.click("cancel")
    b.settle()
    same(snap(b), before, "«Отмена» зеркала")
    b.click("feature.workPlane")
    b.settle()
    b.click_world(30, -30, 0)
    b.fill("value", 12)
    b.settle()
    b.click("cancel")
    b.settle()
    same(snap(b), before, "«Отмена» рабочей плоскости")
    b.key("Escape")
    clean(b, "после отмен")


def suppress_delete_marker(b, steps):
    tree = b.run("part.tree")["features"]
    ids = {f["name"]: f["id"] for f in tree}
    final = steps[-1]
    # suppression: the fillet off = the state before it with the chamfer on; on again = exactly as it was
    b.run("part.suppress", id=ids["Сопряжение1"])
    b.settle()
    off = snap(b)
    check(off[0] > final[0] or off[0] != final[0], "подавление сопряжения ничего не изменило")
    check([f for f in off[4] if f[1] == "Сопряжение1"][0][4], "сопряжение не отмечено подавленным")
    check(not b.errors_on_screen(), f"подавление: на экране ошибка {b.errors_on_screen()}")
    bad = [f[1] for f in off[4] if f[3] not in ("ok", None) and f[1] != "Сопряжение1"]
    check(not bad, f"подавление сопряжения сломало: {bad}; состояние самого сопряжения {[f[3] for f in off[4] if f[1] == 'Сопряжение1']}")
    b.run("part.suppress", id=ids["Сопряжение1"], suppressed=False)
    b.settle()
    same(snap(b), final, "подавление и включение сопряжения")
    # suppressing the flange's extrusion: what stands after it must not break the program, and the return is exact
    b.run("part.suppress", id=ids["Выдавливание2"])
    b.settle()
    b.run("part.suppress", id=ids["Выдавливание2"], suppressed=False)
    b.settle()
    same(snap(b), final, "подавление и включение выдавливания")
    # deletion of a sketch used by an extrusion: the dependants are listed; undo gives all back
    r = b.run("part.delete", id=ids["Эскиз2"])
    check(r, "удаление эскиза ничего не ответило")
    b.settle()
    b.run("doc.undo")
    b.settle()
    same(snap(b), final, "отмена удаления эскиза")
    # the end marker: rolled back to after the hole = the first snapshot with a hole
    names = [f["name"] for f in b.run("part.tree")["features"]]
    b.run("part.set_end_marker", index=names.index("Отверстие1") + 1)
    b.settle()
    now = snap(b)
    check(abs(now[0] - steps[1][0]) < 1e-6, f"«Конец детали» после отверстия: объём {now[0]}, ждали {steps[1][0]}")
    b.run("part.set_end_marker")
    b.settle()
    check(abs(snap(b)[0] - final[0]) < 1e-6, "«Конец детали» в конец: объём не вернулся")


def fresh(new, **params):
    """The same part built from scratch by commands with the parameters set at once: what a rebuild must give."""
    b = new()
    build(b)
    for name, value in params.items():
        b.run("part.set_parameter", name=name, value=value)
    b.settle()
    return props(b)


def edit_by_double_click(b):
    """A double click on a row opens the operation; a new distance rebuilds everything after it."""
    n = len(b.run("part.tree")["features"])
    b.click("tree:Выдавливание1", double=True)
    b.settle()
    check(b.state().get("operation"), "двойной щелчок по строке не открыл правку")
    b.fill("Расстояние", 8)
    b.settle()
    b.click("done")
    b.settle()
    clean(b, "правка выдавливания")
    check(len(b.run("part.tree")["features"]) == n, "правка добавила или потеряла операции")
    return props(b)


def edit_sketch_dimension(b):
    """Double click on the sketch row (under its extrusion): the sketch opens; finishing it unchanged changes nothing.
    A dimension of the sketch changed afterwards rebuilds the operations that stand later."""
    before = snap(b)
    b.click_at(82, 276)                       # the arrow of the first extrusion: its sketch shows
    b.settle()
    b.click("tree:Эскиз1", double=True)
    b.settle()
    check(b.state().get("mode") == "sketch", "двойной щелчок по эскизу не открыл его")
    b.click("pinned-finish")
    b.settle()
    same(snap(b), before, "открыть и закрыть эскиз без правок")
    b.run("part.set_parameter", name="d1", value=70)
    b.settle()
    clean(b, "размер эскиза")


def variables_panel(b):
    """The fx panel: a new variable, its rename, a size tied to it; wrong names and expressions give a message."""
    def put(label, nth, text):
        it = b.find(label, nth=nth)
        b.click_at(*b.center(it))
        b.key("ctrl+a", pause=0.1)
        b.type(text)
        b.key("Return")
        b.settle()

    def parameter(name):
        return next(p for p in b.run("part.parameters") if p["name"] == name)

    b.click("##rail-vars")
    b.settle()
    b.click("##add-var")
    b.settle()
    put("##name", 0, "w")
    check(parameter("w")["expression"] == "10", "новая переменная не получила значение по умолчанию")
    put("##expr", 0, "70")
    put("##expr", 1, "w")                       # the first size: d1 = w
    check(parameter("d1")["expression"] == "w" and abs(parameter("d1")["value"] - 70) < 1e-9, "размер d1 не связался с переменной")
    put("##name", 0, "width")                   # a rename follows into the expressions that use it
    check(parameter("d1")["expression"] == "width", f"переименование не дошло до d1: {parameter('d1')['expression']}")
    put("##name", 0, "1x")
    check(any(p["name"] == "width" for p in b.run("part.parameters")), "имя «1x» принято")
    check(b.errors_on_screen(), "для имени «1x» нет сообщения")
    put("##name", 0, "d1")
    check(any(p["name"] == "width" for p in b.run("part.parameters")), "имя «d1» (занято) принято")
    put("##expr", 0, "abc")
    check(not b.problems(), "ошибка в выражении переменной: сбой в журнале")
    put("##expr", 0, "70")
    check(abs(parameter("d1")["value"] - 70) < 1e-9, "после исправления выражения размер не вернулся")
    b.click("##rail-tree")
    b.settle()


def run(new):
    b = new()
    steps = build(b)
    undo_redo(b, steps)
    cancel_leaves_nothing(b)
    suppress_delete_marker(b, steps)
    check(not b.problems(), f"в журнале: {b.problems()[:2]}")
    b = new()
    build(b)
    got = edit_by_double_click(b)
    same_part(got, fresh(new, d3=8), "правка двойным щелчком равна сборке с d3=8")
    b = new()
    build(b)
    edit_sketch_dimension(b)
    same_part(props(b), fresh(new, d1=70), "правка размера эскиза равна сборке с d1=70")
    b = new()
    build(b)
    variables_panel(b)
    bad = [f["name"] for f in b.run("part.tree")["features"] if f.get("state") not in ("ok", None)]
    check(not bad, f"панель переменных: операции с ошибкой {bad}")
    same_part(props(b), fresh(new, d1=70), "размер от переменной равен сборке с d1=70")
    check(not b.problems(), f"в журнале: {b.problems()[:2]}")
