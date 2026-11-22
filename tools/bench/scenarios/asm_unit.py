"""Plate, two posts, cover, two bolts: by commands (the reference), then by hand two ways;
the places and the clashes are compared; undo / redo of the whole chain; save and reopen."""
import tempfile

from _asm import *

PARTS = None


def fresh_parts(new):
    global PARTS
    folder = tempfile.mkdtemp(prefix="asm-unit-")
    PARTS = unit_parts(new(), folder)


def settled_ok(b, what):
    all_satisfied(b, what)
    no_clash(b, what)
    check(not b.problems(), f"{what}: в журнале сбой {b.problems()[:2]}")


def run(new):
    fresh_parts(new)
    b = new()
    want = unit_reference(b, PARTS)
    settled_ok(b, "эталон")
    b = new()
    unit_hand_a(b, PARTS)
    same_places(comps(b), want, "способ А")
    settled_ok(b, "способ А")
    b = new()
    unit_hand_b(b, PARTS)
    same_places(comps(b), want, "способ Б")
    settled_ok(b, "способ Б")
    # undo walks the whole chain back, redo brings it forward again
    n = len(comps(b)["constraints"])
    for i in range(n):
        r = b.run("doc.undo")
        b.settle()
    check(not comps(b)["constraints"], "после отмены всех шагов остались зависимости")
    for i in range(n):
        b.run("doc.redo")
        b.settle()
    same_places(comps(b), want, "после повтора")
    all_satisfied(b, "после повтора")
    # save, reopen
    path = tempfile.mkdtemp(prefix="asm-unit-save-") + "/uzel.bzc"
    b.run("doc.save", path=path)
    b.run("doc.close", discard_changes=True)
    b.run("app.open", path=path)
    b.settle()
    same_places(comps(b), want, "после открытия")
    all_satisfied(b, "после открытия")
    check(len(comps(b)["constraints"]) == n, "после открытия число зависимостей другое")
