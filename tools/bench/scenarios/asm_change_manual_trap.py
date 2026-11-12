"""«Ручное редактирование» в окне изменения, затем «Вернуться в сборку»: ждём —
сборка; выходит — программа тут же снова открывает ту же деталь на месте
(очередь st.manual не пуста), выйти нельзя (ни кнопкой, ни Esc, ни командой)."""
import tempfile
import time

from _asm import *


def run(new):
    folder = tempfile.mkdtemp(prefix="asm-manual2-")
    files = unit_parts(new(), folder)
    b = new()
    gasket_setup(b, files)
    choose_variant(b, 3)
    time.sleep(1.0)
    b.settle()
    for attempt in range(2):
        for i in range(10):   # the program draws on change only: a mouse move brings the next frame
            b.move(800 + i, 500)
            b.settle()
            if b.find("##return-to-assembly"):
                break
        check(b.state().get("type") == "part", f"не в детали: {b.brief()}")
        b.click("##return-to-assembly")
        b.settle()
        time.sleep(1.0)
        b.settle()
        if b.state().get("type") == "assembly":
            return
    check(False, f"из правки на месте не выйти: после «Вернуться в сборку» снова {b.brief()}")
