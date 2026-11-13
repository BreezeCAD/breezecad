#!/usr/bin/env python3
"""Runs the bench scenarios (tools/bench/scenarios/*.py) and prints one line each.

    python3 tools/bench/run.py [part of a name ...] [--work DIR] [--keep]

A scenario is a module with `run(new)`: `new()` starts a fresh program and
returns its Bench (the one before is closed). It fails by raising; after it
the program's log must be clean (bench.Bench.problems).
"""
from __future__ import annotations

import importlib.util
import json
import sys
import time
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE / "scenarios"))
from bench import Bench, BenchError  # noqa: E402


def monitor_summary(path: Path) -> dict:
    """Slow frames and hangs the program reported about itself (core/Monitor)."""
    out = {"hangs": 0, "worst_ms": 0.0, "frames": 0}
    if not path.exists():
        return out
    for line in path.read_text(errors="replace").splitlines():
        try:
            e = json.loads(line)
        except json.JSONDecodeError:
            continue
        kind = e.get("t") or e.get("type") or e.get("event")
        if kind == "hang":
            out["hangs"] += 1
        if kind == "frame":
            out["frames"] += 1
            out["worst_ms"] = max(out["worst_ms"], float(e.get("ms", 0)))
    return out


def main(argv):
    work = Path("/tmp/bz-bench")
    names = []
    it = iter(argv)
    for a in it:
        if a == "--work":
            work = Path(next(it))
        else:
            names.append(a)
    files = sorted((HERE / "scenarios").glob("*.py"))
    files = [f for f in files if not f.name.startswith("_") and (not names or any(n in f.stem for n in names))]
    failed = 0
    for f in files:
        spec = importlib.util.spec_from_file_location(f.stem, f)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        benches: list[Bench] = []

        def new(file=None, **kw):
            if benches:
                benches[-1].stop()
            b = Bench(file, work=str(work / f.stem / f"run{len(benches) + 1}"), **kw)
            benches.append(b)
            return b.start()

        started = time.time()
        verdict, detail = "ok", ""
        try:
            mod.run(new)
        except Exception as e:  # a scenario fails by raising
            verdict = "FAIL"
            detail = f"{type(e).__name__}: {e}"
            if not isinstance(e, BenchError):
                detail += "\n" + traceback.format_exc(limit=4)
            try:
                if benches:
                    detail += f"\n    снимок: {benches[-1].shot_png('failure')}"
            except Exception:
                pass
        problems = [p for b in benches for p in b.problems()]
        if benches:
            benches[-1].stop()
        if problems and verdict == "ok":
            verdict, detail = "FAIL", "в журнале программы: " + " | ".join(problems[:4])
        elif problems:
            detail += "\n    в журнале программы: " + " | ".join(problems[:4])
        failed += verdict != "ok"
        print(f"{verdict:4} {f.stem:28} {time.time() - started:6.1f} с  {detail}", flush=True)
    print(f"\nсценариев: {len(files)}, провалов: {failed}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
