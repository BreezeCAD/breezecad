"""Окно «Обнаружено изменение»: вариант «Ручное редактирование». Ждём: деталь
открыта на месте и в ней сразу открыт первый затронутый эскиз; потом можно выйти в
сборку. Выходит: деталь открывается, а эскиз — нет, и «Вернуться в сборку» сразу же
заводит обратно в деталь (очередь st.manual не пуста; LeadManualEdit вызывается
из DrawChangeDialog только пока активна сама сборка — а активна уже деталь)."""
import tempfile
import time

from _asm import *


def run(new):
    folder = tempfile.mkdtemp(prefix="asm-manual-")
    files = unit_parts(new(), folder)
    b = new()
    gasket_setup(b, files)
    choose_variant(b, 3)
    time.sleep(1.0)
    b.settle()
    check(b.state().get("type") == "part", f"после «Ручное редактирование» деталь не открылась: {b.brief()}")
    check(b.state().get("mode") == "sketch", f"эскиз не открылся (режим {b.state().get('mode')!r})")
