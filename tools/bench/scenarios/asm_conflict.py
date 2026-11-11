"""Противоречивая зависимость не принимается молча (как в Inventor).

Плита (закреплена) и блок 15 мм: «Совмещение» верх плиты — низ блока, зазор 0.
Затем руками второе: верх плиты — верх блока, зазор 30 (при любом развороте
блока противоречит первому): «Применить»
не принимает её — в панели «противоречит уже наложенным: Совмещение:1» и
кнопка «Всё равно применить»; «Отмена» — блок там, где был, первая зависимость
выполнена. Тот же конфликт и «Всё равно применить» — она в сборке."""
import tempfile

from _asm import *


def setup(b, files):
    new_assembly(b)
    b.run("assembly.place", file=files["plita"])
    b.run("assembly.place", file=files["blok"], at=[0, 0, 40])
    b.settle()
    look(b, (0.3, -1, 0.15))


def run(new):
    folder = tempfile.mkdtemp(prefix="asm-conflict-")
    b = new()
    files = {"plita": make_plate(b, folder), "blok": make_block(b, folder)}
    b = new()
    setup(b, files)
    # The first by command (the block's bottom then lies on the plate, nothing to click);
    # the second, the one judged, by hand in the panel.
    top = plane_of(b, 1, (0, 0, 1))
    bottom = plane_of(b, 2, (0, 0, -1))
    r = constrain(b, "mate", top, bottom, value=0)
    check(r["state"] == "satisfied", f"первая зависимость: {r['state']}")
    z0 = origin(b, 2)[2]

    # The second, contradicting one, through the panel.
    tool(b, "assembly.mate")
    b.click_world(30, 0, 10)       # the plate's top
    b.settle()
    b.click_world(0, 0, z0 + 15)   # the block's top
    b.settle()
    con_value(b, 30)
    b.click("##con-apply")
    b.settle()
    check(b.find("##con-force"), "противоречивая зависимость принята молча: нет «Всё равно применить»")
    b.click("##con-cancel")
    b.settle()
    check(abs(origin(b, 2)[2] - z0) < 1e-6, f"после «Отмены» блок сдвинут: z={origin(b, 2)[2]}, был {z0}")
    all_satisfied(b, "после отмены противоречивой")

    # Again, kept anyway.
    tool(b, "assembly.mate")
    b.click_world(30, 0, 10)
    b.settle()
    b.click_world(0, 0, z0 + 15)
    b.settle()
    con_value(b, 30)
    b.click("##con-apply")
    b.settle()
    b.click("##con-force")
    b.settle()
    check(not b.find("##con-force") and "operation" not in b.brief(), "«Всё равно применить» не закрыло панель")
