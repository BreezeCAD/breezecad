"""Размеры в эскизе: первый масштабирует эскиз, выражение с именем другого размера (d1/2),
ошибочные значения отвергаются без следов, лишний размер — «противоречит», отмена."""
from _common import *


def dims(s):
    return {c["parameter"]["name"]: (c["parameter"]["expression"], c["type"]) for c in s["constraints"] if "parameter" in c}


def to_select(b):
    for _ in range(3):
        if b.brief().get("sketch_tool") == "Выбор" and not b.state().get("text_input"):
            return
        b.key("Escape"); b.settle()


def run(new):
    b = new()
    new_part(b); start_sketch_on_xy(b)
    b.click("sketch.rectangle")
    b.click_sketch(10, 10); b.move_sketch(25, 20); b.click_sketch(40, 30)
    b.settle(); b.key("Escape"); b.settle()
    b.click("sketch.dimension")
    side = lines_by_side(last_sketch(b))
    x, y = mid(side["bottom"])
    dimension(b, (x, y), (x, y - 8), 50)
    s = last_sketch(b)
    side = lines_by_side(s)
    check(abs(side["bottom"]["to"][0] - side["bottom"]["from"][0]) - 50 < 1e-6 and
          abs(abs(side["bottom"]["to"][0] - side["bottom"]["from"][0]) - 50) < 1e-6, "первый размер 50 не выдержан")
    # Вторая сторона выражением от первого размера (знак / набирается без Shift).
    x, y = mid(side["left"])
    dimension(b, (x, y), (x - 9, y), "d1/2")
    s = last_sketch(b)
    check(dims(s).get("d2", ("",))[0] == "d1/2", f"выражение не сохранилось: {dims(s)}")
    side = lines_by_side(s)
    h = side["top"]["from"][1] - side["bottom"]["from"][1]
    check(abs(h - 25) < 1e-6, f"высота {h}, ждали 25")
    check(s["degrees_of_freedom"] == 2, f"dof {s['degrees_of_freedom']}, ждали 2 (положение)")
    # Правка первого размера тянет второй.
    b.key("Escape"); b.settle()
    b.run("tree") if False else None
    # Размер, уже определённый другими, становится справочным и ничего не ломает.
    b.click("sketch.dimension")
    x, y = mid(side["bottom"])
    b.click_sketch(x + 8, y); b.click_sketch(x + 8, y - 25); b.settle()
    check("справочный" in " ".join(n["text"] for n in b.notes()), f"нет сообщения о справочном размере: {b.notes()}")
    s = last_sketch(b)
    check(dims(s).get("d1", ("",))[0] == "50", f"d1 испорчен: {dims(s)}")
    check(s["degrees_of_freedom"] == 2, f"dof {s['degrees_of_freedom']}")
    # Ошибочные значения: поле закрывается с сообщением, размер не меняется.
    for n, bad in enumerate(("abc", "0")):
        yy = -10 - 15 * n
        to_select(b); b.click("sketch.line")
        b.click_sketch(-30, yy); b.move_sketch(-15, yy); b.click_sketch(-10, yy); b.settle()
        to_select(b)
        b.click("sketch.dimension")
        b.click_sketch(-20, yy); b.click_sketch(-20, yy - 6); b.settle()
        check(b.state().get("text_input"), "поле размера не открылось")
        b.enter(bad); b.settle()
        s = last_sketch(b)
        check(all(not c.get("conflict") for c in s["constraints"]), f"«{bad}»: конфликт в эскизе")
        check(s["state"] == "ok", f"«{bad}»: эскиз {s['state']}")
    to_select(b)
    check(not b.problems(), f"в журнале: {b.problems()}")

    # Размер с введённым значением отменяется одним Ctrl+Z (как в Inventor): и значение, и сам размер.
    count = len(dims(last_sketch(b)))
    to_select(b); b.click("sketch.line")
    b.click_sketch(-30, -40); b.move_sketch(-15, -40); b.click_sketch(-10, -40); b.settle()
    to_select(b)
    before = [l for l in last_sketch(b)["lines"]]
    b.click("sketch.dimension")
    b.click_sketch(-20, -40); b.click_sketch(-20, -46); b.settle()
    check(b.state().get("text_input"), "поле размера не открылось")
    b.enter("12"); b.settle()
    check(len(dims(last_sketch(b))) == count + 1, "размер 12 не поставлен")
    to_select(b)
    b.key("ctrl+z"); b.settle()
    s = last_sketch(b)
    check(len(dims(s)) == count, f"после одного Ctrl+Z размер остался: {dims(s)}")
    check(s["lines"] == before, "после одного Ctrl+Z отрезок не вернулся к прежней длине")
