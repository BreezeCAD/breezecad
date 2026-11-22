"""В плите удалено ОДНО отверстие (то, что держит стойку 2); отверстие стойки 1
не тронуто. Ждали (Inventor): зависимость, потерявшая ссылку, помечена,
остальные зависимости и все детали остаются как были (стойка 1 на месте).
Выходит: зависимость с потерянной ссылкой участвует в решении как конфликтная —
«не выполняются» ВСЕ пять зависимостей узла, стойки/крышка/болты разъезжаются
в компромиссные положения (x=37.2 / 12.8, наклон), хотя нужны были только
стойка 2 и её вставка."""
import tempfile

from _asm import *


def run(new):
    folder = tempfile.mkdtemp(prefix="asm-lost-")
    files = unit_parts(new(), folder)
    b = new()
    unit_reference(b, files)
    before = comps(b)
    b.run("assembly.edit_in_place", component=1)
    b.settle()
    sk = next(f["id"] for f in b.run("part.tree")["features"] if f["type"] == "sketch")
    hole2 = next(c["id"] for c in b.run("sketch.get", sketch=sk)["circles"] if c["at"][0] < 0)
    b.run("sketch.remove", sketch=sk, entities=[hole2])
    b.settle()
    b.run("assembly.return_to_assembly")
    b.settle()
    after = comps(b)
    ref = {c["id"]: c for c in before["components"]}
    moved = [c["name"] for c in after["components"]
             if c["id"] != 3 and math.dist(c["placement"]["origin"], ref[c["id"]]["placement"]["origin"]) > 0.01]
    check(not moved, f"компоненты, не связанные с удалённым отверстием, сдвинулись: {moved}")
    st = {k["id"]: k["state"] for k in after["constraints"]}
    check(st.get(7) == "satisfied", f"Вставка:1 (отверстие цело) стала {st.get(7)}")
