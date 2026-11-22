"""Перетаскивание компонента мышью: свободного, частично связанного, закреплённого,
связанной цепочки; закрепление кнопкой ленты; отмена перетаскивания."""
import tempfile

from _asm import *


def drag_at(b, pt, delta, steps=10):
    x, y = b.pixel(*pt)
    b.move(x, y)
    b.settle()
    b.drag(x, y, x + delta[0], y + delta[1], steps=steps)
    b.settle()


def run(new):
    folder = tempfile.mkdtemp(prefix="asm-drag-")
    b = new()
    files = unit_parts(b, folder)
    plate4 = files["plita"]
    b = new()
    new_assembly(b)
    b.run("assembly.place", file=files["plita"])
    b.run("assembly.place", file=files["bolt"], at=[100, 0, 30])
    b.run("assembly.place", file=files["stoyka"], at=[-100, 0, 0])
    b.settle()
    look(b, (0.3, -1, 0.6))
    start = {c["id"]: c["placement"]["origin"] for c in comps(b)["components"]}
    ids = list(start)
    plate, bolt, post = ids
    # закреплённая плита
    drag_at(b, (0, 0, 10), (80, 0))
    check(origin(b, plate) == start[plate], f"закреплённая плита сдвинулась: {origin(b, plate)}")
    # свободная стойка
    drag_at(b, (-110, 0, 15), (-60, 40))
    moved = math.dist(origin(b, post), start[post])
    check(moved > 20, f"свободная стойка почти не сдвинулась: {moved:.1f} мм")
    b.run("doc.undo"); b.settle()
    check(math.dist(origin(b, post), start[post]) < 1e-3, "отмена перетаскивания не вернула стойку")
    b.run("doc.redo"); b.settle()
    check(math.dist(origin(b, post), start[post]) > 20, "повтор перетаскивания")
    # закрепить стойку кнопкой ленты (выбрать щелчком по ней) — больше не двигается
    px, py = b.pixel(*origin(b, post)[:2], origin(b, post)[2] + 15)
    b.click_at(px, py); b.settle()
    b.click("assembly.ground"); b.settle()
    check(comp(b, post)["grounded"], "кнопка «Закрепить» не закрепила выбранный компонент")
    where = origin(b, post)
    drag_at(b, (where[0], where[1], where[2] + 15), (60, 30))
    check(math.dist(origin(b, post), where) < 1e-6, "закреплённая стойка сдвинулась")
    b.click_at(px, py); b.settle()
    b.click("assembly.ground"); b.settle()   # переключатель: освободить
    check(not comp(b, post)["grounded"], "повторное нажатие не освободило")
    # болт, вставленный в отверстие: тянется, но остаётся на оси (только поворот)
    constrain(b, "insert", E(b, plate, [25, 0, 10], 4.5), E(b, bolt, [100, 0, 30], 4))
    held = origin(b, bolt)
    drag_at(b, (held[0], held[1], held[2] + 3), (100, -80))
    check(math.dist(origin(b, bolt), held) < 1e-3, f"болт в отверстии уехал с оси: {origin(b, bolt)} вместо {held}")
    all_satisfied(b, "после перетаскивания болта")
    # цепочка узла: тянем крышку, ничего не разлетается
    b = new()
    unit_reference(b, files)
    before = show_state(b)
    b.run("view.set", direction=[0.3, -1, 0.6]); b.settle()
    drag_at(b, (0, -20, 46), (150, -60))
    check(before == show_state(b), "перетаскивание полностью определённого узла сдвинуло детали")
    all_satisfied(b, "узел после перетаскивания")
    check(not b.problems(), f"в журнале сбой {b.problems()[:2]}")
