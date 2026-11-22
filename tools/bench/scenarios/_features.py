"""Shared by the part_* scenarios: finding model edges and faces by place, drawing on the grid, turning the view."""
from __future__ import annotations

import math

from _common import *


def edge_at(b, mid, tol=1e-3) -> int:
    """The number (part.edges) of the edge whose middle is at `mid`."""
    for e in b.run("part.edges")["edges"]:
        if all(abs(e["middle"][i] - mid[i]) < tol for i in range(3)):
            return e["edge"]
    raise BenchError(f"нет ребра с серединой {mid}")


def face_at(b, normal, point, tol=1e-6) -> int:
    """The plane face with this outward normal that holds `point`."""
    for f in b.run("part.faces")["faces"]:
        if f["kind"] != "plane" or any(abs(f["normal"][i] - normal[i]) > 1e-6 for i in range(3)):
            continue
        lo, hi = f["box"]["min"], f["box"]["max"]
        if all(lo[i] - tol <= point[i] <= hi[i] + tol for i in range(3)):
            return f["face"]
    raise BenchError(f"нет грани с нормалью {normal} в точке {point}")


def zoom_out(b, clicks=3):
    """A new sketch fits its plane to the view; zoom out so that nothing is under the view cube."""
    x0, y0, x1, y1 = b.scene()
    b.scroll((x0 + x1) / 2, (y0 + y1) / 2, -clicks)
    b.settle()


def rect_clicks(b, p, q):
    """Rectangle tool from the ribbon: two corners on the grid."""
    b.click("sketch.rectangle")
    b.move_sketch(p[0] + 1, p[1] + 1)
    b.click_sketch(*p)
    b.move_sketch(q[0] - 1, q[1] - 1)
    b.click_sketch(*q)
    b.settle()
    b.key("Escape")


def finish_clicked(b):
    """Принять эскиз: the tool is already off, the button is on screen."""
    b.click("pinned-finish")
    b.settle()
    check(b.state().get("mode") == "model", "эскиз не закрылся")


def extrude_ribbon(b, distance, *, cut=False, through=False):
    b.click("feature.extrude")
    b.settle()
    if cut:
        b.click("Вырезать из тела")
    if through:
        b.click("depth|Насквозь", exact=False)
    else:
        b.fill("Расстояние", distance)
    b.click("done")
    b.settle()


def turn_view(b, dx=-350):
    """The right button drags the view round (to reach the edges at the back)."""
    x0, y0, x1, y1 = b.scene()
    b.drag((x0 + x1) / 2 + 200, y1 - 60, (x0 + x1) / 2 + 200 + dx, y1 - 60, button=3)
    b.settle()


def tree_names(b, kind=None):
    return [f["name"] for f in b.run("part.tree")["features"] if kind is None or f["type"] == kind]


def uv_of(sk: dict, world) -> list:
    """The sketch coordinates (u, v) of a model point, from what sketch.create said about the plane."""
    p = sk["plane"]
    d = [world[i] - p["origin_in_model"][i] for i in range(3)]
    return [sum(d[i] * p["sketch_x_in_model"][i] for i in range(3)), sum(d[i] * p["sketch_y_in_model"][i] for i in range(3))]
