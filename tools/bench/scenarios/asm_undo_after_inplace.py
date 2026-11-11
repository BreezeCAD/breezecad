"""Правка детали на месте (двойной щелчок в дереве), возврат в сборку, затем
Ctrl+Z. Ждём: либо правка детали отменена целиком, либо сборка остаётся
согласованной (стойка стоит на плите, коллизий нет, плита та же, что и стойка
«думает»). Выходит: отмена откатывает чужой шаг (последнюю зависимость) и
ПОЛОЖЕНИЯ компонентов прежних времён, а плита остаётся утолщённой: стойка
торчит внутри плиты (коллизии), зависимости при этом «выполнены»
(ReturnToAssembly не ставит точку отмены сборки; Undo возвращает старые
положения и не пересчитывает по новой детали)."""
import tempfile

from _asm import *


def run(new):
    folder = tempfile.mkdtemp(prefix="asm-undo-inplace-")
    files = unit_parts(new(), folder)
    b = new()
    unit_reference(b, files)
    no_clash(b, "до правки")
    b.click("tree:plita:1", double=True)
    b.settle()
    check(b.state().get("type") == "part", f"двойной щелчок не открыл правку на месте: {b.brief()}")
    b.run("part.set_parameter", name="d5", value=15)
    b.settle()
    b.run("assembly.return_to_assembly")
    b.settle()
    check(abs(comp(b, 2)["placement"]["origin"][2] - 15) < 1e-3, "после правки стойка не поднялась на 15")
    b.key("ctrl+z")
    b.settle()
    plate_top = comp(b, 1)["box"]["max"][2]
    post_base = comp(b, 2)["placement"]["origin"][2]
    check(abs(plate_top - post_base) < 0.01 or abs(plate_top - 10) < 0.01 and abs(post_base - 10) < 0.01,
          f"после Ctrl+Z плита до z={plate_top:.2f}, а стойка стоит на z={post_base:.2f}: сборка рассогласована")
    no_clash(b, "после Ctrl+Z")
