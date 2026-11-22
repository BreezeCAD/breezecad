"""Окно «Обнаружено изменение»: «Игнорировать» → Отмена → Повтор. Ждём: после
Повтора деталь снова не следует за источником (габарит по X ±40, как после
решения). Выходит: после Отмены деталь молча подтянулась к новому контуру
(±50), а Повтор решение «Игнорировать» не возвращает — ±50 остаётся."""
import tempfile

from _asm import *


def run(new):
    folder = tempfile.mkdtemp(prefix="asm-redo-")
    files = unit_parts(new(), folder)
    b = new()
    gasket_setup(b, files)
    choose_variant(b, 0)
    check(abs(gasket_box(b)["max"][0] - 40) < 0.01, f"после «Игнорировать» деталь изменилась: {gasket_box(b)}")
    b.run("doc.undo")
    b.settle()
    b.run("doc.redo")
    b.settle()
    got = gasket_box(b)["max"][0]
    check(abs(got - 40) < 0.01, f"после Отмены и Повтора «Игнорировать» деталь стала шире (max X = {got}), решение не повторено")
