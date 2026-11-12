"""Вал во втулке: соединение «Цилиндрическое» по кромкам — кромка втулки R4.5 и кромка
болта R4 там, где стержень входит в головку (z=0 детали). Ждали: оси болта и втулки
совпали (болт стоит в x=y=0). Выходит: состояние «выполнено», а болт стоит в x=4.0 —
ось сдвинута на радиус (с кромкой головки R7 или торцевой R4 — на оси; с «Вращением» по
той же паре кромок — на оси). Так же ведёт себя «Ползун»."""
import math
import tempfile

from _asm import *


def run(new):
    folder = tempfile.mkdtemp(prefix="asm-cyl-")
    b = new()
    post = make_tube(b, folder, inner=9)
    bolt = make_bolt(b, folder)
    b = new()
    new_assembly(b)
    b.run("assembly.place", file=post)
    b.run("assembly.place", file=bolt, at=[100, 0, 40])
    b.settle()
    r = constrain(b, "cylindrical", E(b, 1, [0, 0, 30], 4.5), E(b, 2, [100, 0, 40], 4))
    p = comp(b, 2)["placement"]["origin"]
    check(r["state"] == "satisfied", f"состояние {r['state']}")
    check(math.hypot(p[0], p[1]) < 1e-3, f"ось болта не на оси втулки: начало детали {p}, состояние «{r['state']}»")
