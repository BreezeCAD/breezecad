#!/usr/bin/env python3
"""BreezeCAD test bench: drives the program the way a person does.

Real mouse and keys go through X (Xvfb, xdotool); what is on screen and
where comes from the program itself (src/app/testing/UiProbe: BZ_PROBE_DIR),
so a scenario names a button or a model point, never a pixel:

    from bench import Bench
    with Bench() as b:
        b.click("file.newPart")              # a ribbon/home action by its id
        b.click("sketch.start")
        b.click_world(40, 0, 20)             # a point of the model, on screen
        b.key("r")
        b.click_sketch(0, 0); b.click_sketch(60, 40)
        b.run("part.properties")             # read back what the hands built

Needs a build with -DBZ_UI_PROBE=ON. Python 3.8+, standard library only.
Every Bench keeps its own settings folder: runs do not depend on each other.
"""
from __future__ import annotations

import json
import os
import shutil
import signal
import subprocess
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent


class BenchError(AssertionError):
    pass


class Bench:
    def __init__(self, file: str | None = None, *, binary: str | None = None, display: str = ":99",
                 size: tuple[int, int] = (1440, 900), work: str | None = None, settings: dict | None = None,
                 env: dict | None = None, lang: str = "ru"):
        self.display = display
        self.size = size
        self.work = Path(work or tempfile.mkdtemp(prefix="bz-bench-"))
        self.probe_dir = self.work / "probe"
        self.shots = self.work / "shots"
        self.home = self.work / "home"
        self.log = self.work / "program.log"
        self.monitor = self.work / "monitor.jsonl"
        self.journal: list[str] = []          # what the hands did, to repeat a failure
        self._id = 0
        self._proc: subprocess.Popen | None = None
        self._file = file
        self._settings = settings or {}
        self._extra_env = env or {}
        self._lang = lang
        b = binary or os.environ.get("BZ_BIN")
        if b:
            self.binary = Path(b) / "breezecad" if Path(b).is_dir() else Path(b)
        else:
            found = [ROOT / "build" / p / "bin" / "breezecad" for p in ("linux-debug", "linux-release", "linux-asan")]
            self.binary = next((p for p in found if p.exists()), found[0])

    # ------------------------------------------------------------ lifecycle
    def __enter__(self):
        self.start()
        return self

    def __exit__(self, kind, value, tb):
        self.stop()

    def _x(self, *args, check=False):
        env = dict(os.environ, DISPLAY=self.display)
        return subprocess.run(["xdotool", *map(str, args)], env=env, capture_output=True, text=True, check=check)

    def start(self):
        # One program per display: a leftover of a failed run would answer too.
        subprocess.run(["pkill", "-x", "breezecad"], capture_output=True)
        time.sleep(0.3)
        shutil.rmtree(self.probe_dir, ignore_errors=True)
        for d in (self.probe_dir, self.shots, self.home / ".config" / "breezecad"):
            d.mkdir(parents=True, exist_ok=True)
        if self._settings:
            (self.home / ".config" / "breezecad" / "settings.json").write_text(json.dumps(self._settings), encoding="utf-8")
        if subprocess.run(["pgrep", "-f", f"Xvfb {self.display} "], capture_output=True).returncode != 0:
            subprocess.Popen(["Xvfb", self.display, "-screen", "0", "1600x1000x24"],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(0.7)
        env = dict(os.environ, DISPLAY=self.display, HOME=str(self.home), XDG_CONFIG_HOME=str(self.home / ".config"),
                   XDG_DATA_HOME=str(self.home / ".local" / "share"), XDG_CACHE_HOME=str(self.home / ".cache"),
                   LIBGL_ALWAYS_SOFTWARE="1", BZ_NO_TOUCHPAD="1", BZ_NO_SPLASH="1", LANG="C.UTF-8",
                   LANGUAGE=self._lang, BZ_PROBE_DIR=str(self.probe_dir), BZ_SHOT_DIR=str(self.shots),
                   BZ_MONITOR=str(self.monitor))
        env.update(self._extra_env)
        args = [str(self.binary)] + ([self._file] if self._file else [])
        self._proc = subprocess.Popen(args, cwd=str(self.binary.parent), env=env, stdout=open(self.log, "w"),
                                      stderr=subprocess.STDOUT)
        deadline = time.time() + 40
        while time.time() < deadline:
            if self._x("search", "--name", "BreezeCAD").stdout.strip():
                break
            self._alive()
            time.sleep(0.25)
        else:
            raise BenchError("the window did not appear")
        win = self._x("search", "--name", "BreezeCAD").stdout.split()[0]
        self._x("windowmove", win, 0, 0)
        self._x("windowsize", win, *self.size)
        time.sleep(0.8)
        self.state()   # the probe answers: the program is up
        return self

    def stop(self):
        if self._proc and self._proc.poll() is None:
            self._proc.send_signal(signal.SIGTERM)
            try:
                self._proc.wait(5)
            except subprocess.TimeoutExpired:
                self._proc.kill()
        self._proc = None

    def _alive(self):
        if self._proc is None or self._proc.poll() is not None:
            code = None if self._proc is None else self._proc.returncode
            raise BenchError(f"the program is gone (exit code {code}); last steps: {self.journal[-6:]}")

    # ---------------------------------------------------------------- probe
    def probe(self, op: str, timeout: float = 120.0, **args):
        self._id += 1
        reply_file = self.probe_dir / "reply.json"
        if reply_file.exists():
            reply_file.unlink()
        tmp = self.probe_dir / "request.tmp"
        tmp.write_text(json.dumps({"id": self._id, "op": op, **args}), encoding="utf-8")
        tmp.rename(self.probe_dir / "request.json")
        deadline = time.time() + timeout
        while time.time() < deadline:
            if reply_file.exists():
                try:
                    reply = json.loads(reply_file.read_text(encoding="utf-8"))
                except json.JSONDecodeError:
                    time.sleep(0.01)
                    continue
                if reply.get("id") == self._id:
                    if not reply.get("ok"):
                        raise BenchError(f"probe {op}: {reply.get('error')}")
                    return reply
            self._alive()
            time.sleep(0.01)
        raise BenchError(f"probe {op}: no answer in {timeout} s (the program hangs?); last steps: {self.journal[-6:]}")

    def state(self) -> dict:
        return self.probe("state")["state"]

    def items(self, contains: str = "") -> list[dict]:
        return self.probe("items", contains=contains)["items"]

    def run(self, command: str, **args):
        """A scripting command (app/scripting): the result, or BenchError."""
        self.journal.append(f"run {command} {json.dumps(args, ensure_ascii=False)[:120]}")
        reply = self.probe("run", request={"command": command, "args": args})["reply"]
        if not reply.get("ok"):
            raise BenchError(f"{command}: {reply.get('error')}")
        return reply.get("result")

    def try_run(self, command: str, **args):
        try:
            return self.run(command, **args)
        except BenchError as e:
            return {"error": str(e)}

    # ----------------------------------------------------------- finding
    @staticmethod
    def _text(label: str) -> str:
        return label.split("##")[0].replace("\n", " ").strip()

    def find(self, label: str, *, window: str | None = None, nth: int = 0, exact: bool = True,
             enabled: bool | None = None) -> dict | None:
        """An item by its id ("feature.extrude"), its text ("Готово") or its
        whole label; `window`: part of the ImGui window name."""
        hits = []
        for it in self.items():
            raw = it["label"]
            names = {raw, raw.split("##")[-1], self._text(raw)}
            if ":" in raw:   # named by the program: "kind:id|caption"
                rest = raw.split(":", 1)[1]
                names.add(rest)
                for part in rest.split("|"):
                    names.update((part, part.lstrip("#"), self._text(part)))
            ok = (label in names) if exact else (label in raw)
            if not ok:
                continue
            if window and window not in it["window"]:
                continue
            r = it["rect"]
            if r[2] - r[0] < 2 or r[3] - r[1] < 2:
                continue   # clipped away: not on screen
            if enabled is not None and bool(it.get("disabled")) == enabled:
                continue
            hits.append(it)
        return hits[nth] if len(hits) > nth else None

    def wait_for(self, label: str, timeout: float = 8.0, **kw) -> dict:
        deadline = time.time() + timeout
        while True:
            it = self.find(label, **kw)
            if it:
                return it
            if time.time() > deadline:
                raise BenchError(f"not on screen: {label!r}; last steps: {self.journal[-6:]}")
            time.sleep(0.1)

    def gone(self, label: str, timeout: float = 8.0, **kw):
        deadline = time.time() + timeout
        while self.find(label, **kw):
            if time.time() > deadline:
                raise BenchError(f"still on screen: {label!r}")
            time.sleep(0.1)

    @staticmethod
    def center(item: dict) -> tuple[int, int]:
        r = item["rect"]
        return int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)

    # ------------------------------------------------------------- hands
    def move(self, x, y, pause: float = 0.12):
        self._x("mousemove", int(x) - 2, int(y) - 1)
        time.sleep(0.05)
        self._x("mousemove", int(x), int(y))
        time.sleep(pause)

    def click_at(self, x, y, button: int = 1, double: bool = False, pause: float = 0.25, held: tuple = ()):
        self.journal.append(f"click {int(x)},{int(y)} b{button}{' x2' if double else ''} {'+'.join(held)}")
        self.move(x, y)
        for k in held:
            self._x("keydown", k)
        if double:
            self._x("click", "--repeat", 2, "--delay", 80, button)
        else:
            self._x("click", button)
        for k in held:
            self._x("keyup", k)
        time.sleep(pause)
        self._alive()

    def click(self, label: str, *, button: int = 1, double: bool = False, timeout: float = 8.0, **kw):
        it = self.wait_for(label, timeout, **kw)
        if it.get("disabled"):
            raise BenchError(f"{label!r} is disabled; last steps: {self.journal[-6:]}")
        self.journal.append(f"-> {label}")
        self.click_at(*self.center(it), button=button, double=double)
        return it

    def drag(self, x0, y0, x1, y1, button: int = 1, steps: int = 8, hold: float = 0.1, held: tuple = ()):
        """Press, move, release; `held`: keys kept down meanwhile ("shift")."""
        self.journal.append(f"drag {int(x0)},{int(y0)} -> {int(x1)},{int(y1)} b{button} {'+'.join(held)}")
        self.move(x0, y0)
        for k in held:
            self._x("keydown", k)
        self._x("mousedown", button)
        time.sleep(hold)
        for i in range(1, steps + 1):
            self._x("mousemove", int(x0 + (x1 - x0) * i / steps), int(y0 + (y1 - y0) * i / steps))
            time.sleep(0.03)
        time.sleep(hold)
        self._x("mouseup", button)
        for k in held:
            self._x("keyup", k)
        time.sleep(0.25)
        self._alive()

    def scroll(self, x, y, clicks: int):
        self.move(x, y)
        for _ in range(abs(clicks)):
            self._x("click", 4 if clicks > 0 else 5)
            time.sleep(0.04)
        time.sleep(0.2)

    def key(self, *chords: str, pause: float = 0.25):
        self.journal.append("key " + " ".join(chords))
        for c in chords:
            self._x("key", c)
            time.sleep(pause)
        self._alive()

    def type(self, text: str, pause: float = 0.2):
        self.journal.append(f"type {text!r}")
        self._x("type", "--delay", 40, str(text))
        time.sleep(pause)

    def enter(self, value, select_all: bool = True):
        """Replaces the focused field's text and confirms with Enter."""
        if select_all:
            self.key("ctrl+a", pause=0.1)
        self.type(str(value))
        self.key("Return")

    def fill(self, label: str, value, confirm: str | None = "Tab", **kw):
        """Clicks a field by its label/id and types a value into it."""
        self.click(label, **kw)
        self.key("ctrl+a", pause=0.1)
        self.type(str(value))
        if confirm:
            self.key(confirm)

    # ------------------------------------------------------ model on screen
    def pixel(self, x, y, z) -> tuple[float, float]:
        return tuple(self.probe("project", points=[[x, y, z]])["pixels"][0])

    def sketch_pixel(self, u, v) -> tuple[float, float]:
        return tuple(self.probe("sketch_project", points=[[u, v]])["pixels"][0])

    def _in_view(self, px, py, what: str):
        """A model point a person could not click (off the view, under the
        view cube) is the scenario's mistake: say so instead of clicking the ribbon."""
        x0, y0, x1, y1 = self.scene()
        if not (x0 + 4 <= px <= x1 - 4 and y0 + 4 <= py <= y1 - 4):
            raise BenchError(f"{what} is off the view ({int(px)},{int(py)}): fit the view first")
        for it in self.items("cube"):
            r = it["rect"]
            if it["label"] == "##viewcube" and r[0] - 12 <= px <= r[2] + 12 and r[1] - 12 <= py <= r[3] + 12:
                raise BenchError(f"{what} is under the view cube ({int(px)},{int(py)})")
        return px, py

    def click_world(self, x, y, z, **kw):
        self.click_at(*self._in_view(*self.pixel(x, y, z), f"model point {x},{y},{z}"), **kw)

    def click_sketch(self, u, v, **kw):
        self.click_at(*self._in_view(*self.sketch_pixel(u, v), f"sketch point {u},{v}"), **kw)

    def move_sketch(self, u, v):
        self.move(*self._in_view(*self.sketch_pixel(u, v), f"sketch point {u},{v}"))

    def drag_sketch(self, u0, v0, u1, v1, **kw):
        self.drag(*self._in_view(*self.sketch_pixel(u0, v0), "drag start"), *self._in_view(*self.sketch_pixel(u1, v1), "drag end"), **kw)

    def scene(self) -> list[float]:
        return self.state()["scene"]

    # ------------------------------------------------------------- waiting
    def settle(self, timeout: float = 60.0, quiet: float = 0.25):
        """Waits until rebuilds end and the screen stops changing."""
        deadline = time.time() + timeout
        last, since = None, time.time()
        while time.time() < deadline:
            s = self.state()
            if s.get("rebuilding"):
                last, since = None, time.time()
            elif s["frames"] != last:
                last, since = s["frames"], time.time()
            elif time.time() - since >= quiet:
                return s
            time.sleep(0.05)
        raise BenchError(f"the program did not settle in {timeout} s; last steps: {self.journal[-6:]}")

    # ---------------------------------------------------------- evidence
    def shot(self, name: str) -> Path:
        ppm = self.shots / f"{name}.ppm"
        if ppm.exists():
            ppm.unlink()
        (self.shots / "request").write_text(name, encoding="utf-8")
        deadline = time.time() + 15
        while not ppm.exists():
            if time.time() > deadline:
                raise BenchError("no screenshot")
            self._x("mousemove_relative", 0, 0)
            time.sleep(0.1)
        time.sleep(0.15)
        return ppm

    def shot_png(self, name: str) -> Path:
        from imageutil import ppm_to_png
        return ppm_to_png(self.shot(name), self.shots / f"{name}.png")

    def log_lines(self, word: str) -> list[str]:
        try:
            return [l.rstrip() for l in open(self.log, errors="replace") if word in l]
        except FileNotFoundError:
            return []

    def problems(self) -> list[str]:
        """What must never be in a run's log: the safety net at work, errors of the program itself."""
        out = []
        for word in ("Recovered:", "runtime error:", "AddressSanitizer", "FATAL", "Assertion"):
            out += self.log_lines(word)
        return out

    def notes(self) -> list[dict]:
        return self.state().get("notes", [])

    def errors_on_screen(self) -> list[str]:
        return [n["text"] for n in self.notes() if n["kind"] == "error"]


def check(cond, message: str):
    if not cond:
        raise BenchError(message)


def close(a: float, b: float, rel: float = 1e-4, abs_: float = 1e-6) -> bool:
    return abs(a - b) <= max(abs_, rel * max(abs(a), abs(b)))
