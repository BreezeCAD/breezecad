"""Helpers of the assembly scenarios: parts made by commands and saved to files
(the file dialogs are system ones, out of reach on the virtual screen), an
assembly read back, components compared."""
from __future__ import annotations

import math
from pathlib import Path

from bench import Bench, BenchError, check, close


def _save(b: Bench, folder: Path, name: str) -> str:
    path = str(Path(folder) / f"{name}.bzc")
    b.run("doc.save", path=path)
    b.run("doc.close", discard_changes=True)
    return path


def make_plate(b: Bench, folder, name="plita", w=80, d=50, t=10, holes=((-25, 0), (25, 0)), hole_d=8) -> str:
    """A plate w x d x t centred on the origin, bottom on Z=0, through holes."""
    b.run("app.new_part")
    sk = b.run("sketch.create", plane="XY")["id"]
    # Holes are circles of the same sketch: part.faces may answer with the faces of a part closed before.
    items = [{"type": "rectangle", "center": [0, 0], "width": w, "height": d}]
    items += [{"type": "circle", "center": [x, y], "diameter": hole_d} for x, y in holes]
    b.run("sketch.add", sketch=sk, items=items)
    b.run("feature.extrude", sketch=sk, distance=t)
    b.settle()
    return _save(b, folder, name)


def make_tube(b: Bench, folder, name="stoyka", outer=20, inner=8, h=30) -> str:
    """A post: a cylinder on the origin along Z with an axial through hole (inner=0: solid)."""
    b.run("app.new_part")
    sk = b.run("sketch.create", plane="XY")["id"]
    items = [{"type": "circle", "center": [0, 0], "diameter": outer}]
    if inner:
        items.append({"type": "circle", "center": [0, 0], "diameter": inner})
    b.run("sketch.add", sketch=sk, items=items)
    b.run("feature.extrude", sketch=sk, distance=h)
    b.settle()
    return _save(b, folder, name)


def make_bolt(b: Bench, folder, name="bolt", d=8, length=50, head_d=14, head_h=6) -> str:
    """A «bolt»: the shank along Z from 0 down to -length, the head from 0 up to head_h."""
    b.run("app.new_part")
    sk = b.run("sketch.create", plane="XY")["id"]
    b.run("sketch.add", sketch=sk, items=[{"type": "circle", "center": [0, 0], "diameter": head_d}])
    b.run("feature.extrude", sketch=sk, distance=head_h)
    sk2 = b.run("sketch.create", plane="XY")["id"]
    b.run("sketch.add", sketch=sk2, items=[{"type": "circle", "center": [0, 0], "diameter": d}])
    b.run("feature.extrude", sketch=sk2, distance=length, reversed=True, operation="join")
    b.settle()
    return _save(b, folder, name)


def make_block(b: Bench, folder, name="blok", w=30, d=20, h=15) -> str:
    b.run("app.new_part")
    sk = b.run("sketch.create", plane="XY")["id"]
    b.run("sketch.add", sketch=sk, items=[{"type": "rectangle", "center": [0, 0], "width": w, "height": d}])
    b.run("feature.extrude", sketch=sk, distance=h)
    b.settle()
    return _save(b, folder, name)


def new_assembly(b: Bench):
    b.run("assembly.new")
    b.settle()
    check(b.state().get("type") == "assembly", "новая сборка не открылась")


def comps(b: Bench) -> dict:
    return b.run("assembly.components")


def comp(b: Bench, cid: int) -> dict:
    return next(c for c in comps(b)["components"] if c["id"] == cid)


def face(b: Bench, cid: int, kind="plane", normal=None, near=None) -> dict:
    args = {"component": cid, "kind": kind, "limit": 0}
    if normal:
        args["normal"] = normal
    if near:
        args["near"] = near
    faces = b.run("assembly.faces", **args)
    key = "faces" if kind != "edge" else "edges"
    return faces


def bounds(c: dict):
    for k in ("bounds_mm", "bounds", "box"):
        if k in c:
            return c[k]
    raise BenchError(f"нет габарита у компонента: {list(c)}")


def same_boxes(b_got: dict, b_want: dict, what: str, tol=1e-3):
    """Two ways to assemble one unit: every component stands where the reference has it."""
    g, w = b_got["components"], b_want["components"]
    check(len(g) == len(w), f"{what}: компонентов {len(g)}, а в эталоне {len(w)}")
    for cg, cw in zip(g, w):
        bg, bw = bounds(cg), bounds(cw)
        for side in ("min", "max"):
            for i, axis in enumerate("XYZ"):
                check(abs(bg[side][i] - bw[side][i]) <= tol,
                      f"{what}: {cg.get('name', cg['id'])} {side} {axis} = {bg[side][i]:.4f}, а в эталоне {bw[side][i]:.4f}")


def no_clash(b: Bench, what: str, tol=1e-3):
    r = b.run("assembly.clashes")
    pairs = [p for p in r.get("clashes", r.get("pairs", [])) if p.get("volume_mm3", p.get("volume", 1)) > tol]
    check(not pairs, f"{what}: коллизии {pairs[:3]}")


def all_satisfied(b: Bench, what: str):
    bad = [c for c in comps(b)["constraints"] if c.get("state") not in ("satisfied", "ok") and not c.get("suppressed")]
    check(not bad, f"{what}: зависимости не выполнены: {[(c.get('name'), c.get('state')) for c in bad]}")


def fid(b: Bench, cid: int, kind: str, near, normal=None) -> int:
    """The number of the component's face of this kind nearest to a point (assembly coordinates)."""
    args = {"component": cid, "kind": kind, "near": list(near), "limit": 0}
    if normal:
        args["normal"] = list(normal)
    faces = b.run("assembly.faces", **args)["faces"]
    check(faces, f"у компонента {cid} нет грани {kind} у точки {near}")
    return faces[0]["face"]


def eid(b: Bench, cid: int, centre, radius=None) -> int:
    """The number of the component's round edge with this centre (and radius)."""
    edges = b.run("assembly.faces", component=cid, limit=0)["round_edges"]
    edges = [e for e in edges if radius is None or abs(e["radius"] - radius) < 1e-3]
    check(edges, f"у компонента {cid} нет круглой кромки R{radius}")
    return min(edges, key=lambda e: math.dist(e["centre"], centre))["edge"]


def F(b, cid, kind, near, normal=None) -> dict:
    return {"component": cid, "face": fid(b, cid, kind, near, normal)}


def E(b, cid, centre, radius=None) -> dict:
    return {"component": cid, "edge": eid(b, cid, centre, radius)}


def constrain(b: Bench, type_: str, first=None, second=None, **kw) -> dict:
    args = dict(type=type_, **kw)
    if first is not None:
        args["first"] = first
    if second is not None:
        args["second"] = second
    r = b.run("assembly.constrain", **args)
    b.settle()
    return r


def origin(b: Bench, cid: int):
    return comp(b, cid)["placement"]["origin"]


def placed(b: Bench, cid: int, want, what: str, tol=1e-3, z_axis=None):
    p = comp(b, cid)["placement"]
    check(math.dist(p["origin"], want) <= tol, f"{what}: компонент {cid} стоит в {p['origin']}, ждали {list(want)}")
    if z_axis is not None:
        check(math.dist(p["z_axis"], z_axis) <= 1e-4, f"{what}: ось Z компонента {cid} {p['z_axis']}, ждали {list(z_axis)}")


def same_places(got: dict, want: dict, what: str, tol=1e-3, box_tol=0.01):
    """Two ways to assemble one unit: every component where the reference has it
    (origin and box; a part may turn about its own axis of symmetry)."""
    g, w = got["components"], want["components"]
    check(len(g) == len(w), f"{what}: компонентов {len(g)}, а в эталоне {len(w)}")
    for cg, cw in zip(g, w):
        name = cg.get("name", cg["id"])
        og, ow = cg["placement"]["origin"], cw["placement"]["origin"]
        check(math.dist(og, ow) <= tol, f"{what}: {name} стоит в {og}, а в эталоне {ow}")
        for side in ("min", "max"):
            for i, axis in enumerate("XYZ"):
                check(abs(cg["box"][side][i] - cw["box"][side][i]) <= box_tol,
                      f"{what}: {name}, габарит {side} {axis} = {cg['box'][side][i]:.4f}, а в эталоне {cw['box'][side][i]:.4f}")


# ---------------------------------------------------------------- by hand
def look(b: Bench, direction, fit=False):
    """Turns the view (the camera is not what is tested: the picks are)."""
    b.run("view.set", direction=list(direction))
    b.settle()


def tool(b: Bench, action: str):
    b.click(action)
    b.settle()


def pick(b: Bench, x, y, z, **kw):
    b.click_world(x, y, z, **kw)
    b.settle()


def con_apply(b: Bench):
    b.click("##con-apply")
    b.settle()


def con_done(b: Bench):
    b.click("##con-done")
    b.settle()


def con_value(b: Bench, value):
    b.fill("##con-value", value)
    b.settle()


# ---------------------------------------------------------------- by hand: unit "plate, two posts, cover, two bolts"
UP = (0.3, -1, 0.7)      # the camera above and in front
DOWN = (0.3, -1, -0.7)   # below and in front
ABOVE = (0.02, -0.05, 1)  # straight from above (posts and bolts stand outside the plate: nothing hides them)
BELOW = (0.02, -0.05, -1)
P_UNIT = {"plita": None, "stoyka1": [60, 60, 40], "stoyka2": [-60, 60, 40], "kryshka": [-140, 0, 20],
          "bolt1": [60, 130, 60], "bolt2": [-60, 130, 60]}


def unit_parts(b: Bench, folder, hole_d=9) -> dict:
    """The five part files of the unit; returns their paths."""
    return {"plita": make_plate(b, folder, hole_d=hole_d), "stoyka": make_tube(b, folder, inner=9),
            "bolt": make_bolt(b, folder), "kryshka": make_plate(b, folder, "kryshka", t=6, hole_d=hole_d)}


def unit_start(b: Bench, files: dict):
    """An assembly with the six components placed apart (ids 1..6), so that every face is in view."""
    new_assembly(b)
    for f, key in (("plita", "plita"), ("stoyka", "stoyka1"), ("stoyka", "stoyka2"), ("kryshka", "kryshka"),
                   ("bolt", "bolt1"), ("bolt", "bolt2")):
        at = P_UNIT[key]
        b.run("assembly.place", file=files[f], **({"at": at} if at else {}))
    b.settle()


def unit_reference(b: Bench, files: dict) -> dict:
    unit_start(b, files)
    constrain(b, "insert", E(b, 1, [25, 0, 10], 4.5), E(b, 2, [60, 60, 40], 4.5))
    constrain(b, "insert", E(b, 1, [-25, 0, 10], 4.5), E(b, 3, [-60, 60, 40], 4.5))
    constrain(b, "mate", F(b, 2, "plane", [25, 0, 40], [0, 0, 1]), F(b, 4, "plane", [-140, 0, 20], [0, 0, -1]))
    constrain(b, "axis_mate", F(b, 2, "cylinder", [25, 0, 25]), F(b, 4, "cylinder", [-115, 0, 23]))
    constrain(b, "axis_mate", F(b, 3, "cylinder", [-25, 0, 25]), F(b, 4, "cylinder", [-25, 0, 43]))
    constrain(b, "insert", E(b, 4, [25, 0, 46], 4.5), E(b, 5, [60, 130, 60], 4))
    constrain(b, "insert", E(b, 4, [-25, 0, 46], 4.5), E(b, 6, [-60, 130, 60], 4))
    return comps(b)


def edge_pick(b: Bench, view, x, y, z):
    """Turn the view, click a round edge at a model point."""
    look(b, view)
    pick(b, x, y, z)


def zpick(b: Bench, view, x, y, z, zoom=6, **kw):
    """Turn the view, zoom with the wheel at the point (so a ring is wider than the edge pick radius), click it."""
    look(b, view)
    px, py = b.pixel(x, y, z)
    b.scroll(px, py, zoom)
    b.settle()
    pick(b, x, y, z, **kw)


def show_state(b: Bench) -> str:
    c = comps(b)
    return "; ".join(f"{x['id']}@{[round(v, 2) for v in x['placement']['origin']]}z{[round(v) for v in x['placement']['z_axis']]}"
                     for x in c["components"]) + " | " + ", ".join(f"{k['id']}:{k['type']}:{k['state']}" for k in c["constraints"])


def unit_hand_a(b: Bench, files: dict):
    """Way A: Вставка for the posts, Совмещение + two Совпадение осей for the cover, Вставка for the bolts."""
    unit_start(b, files)
    tool(b, "assembly.insert")
    zpick(b, ABOVE, 29.5, 0, 10); zpick(b, BELOW, 64.5, 60, 40); con_apply(b)
    zpick(b, ABOVE, -20.5, 0, 10); zpick(b, BELOW, -55.5, 60, 40); con_done(b)
    tool(b, "assembly.mate")
    zpick(b, ABOVE, 32.5, 0, 40); zpick(b, BELOW, -120, 10, 20); con_done(b)
    tool(b, "assembly.axisMate")
    zpick(b, UP, 25, -10, 25); zpick(b, UP, -115, 4.5, 43, zoom=8); con_done(b)
    tool(b, "assembly.axisMate")
    zpick(b, UP, -25, -10, 25); zpick(b, UP, -25, 4.5, 43, zoom=8); con_done(b)
    tool(b, "assembly.insert")
    zpick(b, UP, 29.5, 0, 46); zpick(b, DOWN, 60, 126, 60); con_apply(b)
    zpick(b, UP, -20.5, 0, 46); zpick(b, DOWN, -60, 126, 60); con_done(b)


def unit_hand_b(b: Bench, files: dict):
    """Way B: Совмещение + Совпадение осей for the posts, Вставка + Совпадение осей for the cover,
    Совмещение + Совпадение осей for the bolts."""
    unit_start(b, files)
    tool(b, "assembly.mate")
    zpick(b, BELOW, 67.25, 60, 40); zpick(b, ABOVE, 0, 15, 10); con_apply(b)
    zpick(b, BELOW, -52.75, 60, 40); zpick(b, ABOVE, 0, 15, 10); con_done(b)
    tool(b, "assembly.axisMate")
    zpick(b, UP, 60, 50, 25); zpick(b, UP, 25, 4.5, 8.5, zoom=8); con_apply(b)
    zpick(b, UP, -60, 50, 25); zpick(b, UP, -25, 4.5, 8.5, zoom=8); con_done(b)
    tool(b, "assembly.insert")
    zpick(b, ABOVE, 29.5, 0, 40); zpick(b, BELOW, -110.5, 0, 20); con_done(b)
    tool(b, "assembly.axisMate")
    zpick(b, UP, -25, -10, 25); zpick(b, UP, -25, 4.5, 43, zoom=8); con_done(b)
    tool(b, "assembly.mate")
    zpick(b, BELOW, 65.5, 130, 60, zoom=9); zpick(b, ABOVE, -15, 15, 46); con_apply(b)
    zpick(b, BELOW, -54.5, 130, 60, zoom=9); zpick(b, ABOVE, -15, 15, 46); con_done(b)
    tool(b, "assembly.axisMate")
    zpick(b, UP, 60, 126, 25); zpick(b, UP, 25, 4.5, 43, zoom=8); con_apply(b)
    zpick(b, UP, -60, 126, 25); zpick(b, UP, -25, 4.5, 43, zoom=8); con_done(b)


def two_plates(b: Bench, folder, rotate_second=None):
    """The plate (grounded) and a cover at x=100, ids 1 and 2; returns the folder's files."""
    files = {"plita": make_plate(b, folder), "kryshka": make_plate(b, folder, "kryshka", t=6)}
    return files


def plane_of(b: Bench, cid: int, normal) -> dict:
    """The component's plane face with this normal (assembly coordinates), as a constraint operand."""
    faces = b.run("assembly.faces", component=cid, kind="plane", normal=list(normal), limit=0)["faces"]
    check(faces, f"у компонента {cid} нет плоской грани с нормалью {list(normal)}")
    return {"component": cid, "face": faces[0]["face"]}


def menu_item(b: Bench, nth: int):
    """A context-menu row by its place (the rows carry no names): the popup has just been opened."""
    import time
    end = time.time() + 8
    while True:
        rows = [i for i in b.items() if "Popup" in i["window"] and (i["label"] == "##item" or i["label"].startswith("menu:"))
                and i["rect"][2] - i["rect"][0] > 2]
        if len(rows) > nth or time.time() > end:
            break
        time.sleep(0.1)
    check(len(rows) > nth, "в контекстном меню нет строки №%d" % nth)
    b.click_at(*b.center(rows[nth]))
    b.settle()


def tree_menu(b: Bench, row: str, nth: int):
    b.click(f"tree:{row}", button=3)
    b.settle()
    menu_item(b, nth)


def gasket_setup(b: Bench, files: dict, width=100):
    """The unit plus a part made in the assembly on the cover's top (its four edges projected, extruded 2 mm),
    then the cover is widened to `width` in place: the window «Обнаружено изменение» must open."""
    unit_reference(b, files)
    b.run("assembly.create_part", name="Gasket", plane=plane_of(b, 4, [0, 0, 1]))
    b.settle()
    sk = b.run("sketch.create", plane="XY")["id"]
    for pt in ([40, 0, 46], [-40, 0, 46], [0, 25, 46], [0, -25, 46]):
        b.run("assembly.project_edge", sketch=sk, component=4, point=pt)
    b.run("feature.extrude", sketch=sk, distance=2)
    b.settle()
    b.run("assembly.return_to_assembly")
    b.settle()
    b.click("tree:kryshka:1", double=True)   # by hand: a double click in the tree
    b.settle()
    check(b.state().get("type") == "part", "двойной щелчок по крышке не открыл правку на месте")
    b.run("part.set_parameter", name="d1", value=width)
    b.settle()
    b.run("assembly.return_to_assembly")
    b.settle()
    check(b.find("##change-dialog"), f"окно «Обнаружено изменение» не появилось: {b.brief()}")


def choose_variant(b: Bench, nth: int, apply=True):
    b.click_at(*b.center(b.wait_for("##variant", nth=nth)))
    b.settle()
    if apply:
        b.click("##change-apply")
        b.settle()


def gasket_box(b: Bench) -> dict:
    return next(c for c in comps(b)["components"] if c["name"].lower().startswith("gasket"))["box"]


def click_wait(b: Bench, label: str, tries=12, **kw):
    """The program draws on change only, so the item list may be one frame old: a mouse move brings the next one."""
    fkw = {k: v for k, v in kw.items() if k in ("window", "nth", "exact", "enabled")}
    for i in range(tries):
        if b.find(label, **fkw):
            break
        b.move(800 + i, 500)
        b.settle()
    return b.click(label, **kw)


def open_in_place(b: Bench, row: str):
    """A double click on the tree row; waits for the part to open (tries again once: a double click is timing-bound)."""
    import time
    for attempt in range(3):
        click_wait(b, f"tree:{row}", double=True)
        b.settle()
        end = time.time() + 3
        while time.time() < end:
            if b.state().get("type") == "part":
                return
            time.sleep(0.1)
    raise BenchError(f"двойной щелчок по «{row}» не открыл правку на месте: {b.brief()}")
