"""MO command-line entry point.

    mo eval  <spec.zspec|spec.py> <trace.jsonl>   replay a recorded stream
    mo watch <spec.zspec|spec.py> <session.jsonl> judge a glassport tap log live

`.zspec` files are parsed by the M1 text parser. `.py` files (M0 form) are
imported and must define a module-level `spec` (a rules.Spec). Both resolve
behind the same `load_spec` call.

Exit code: 0 if the session is clean, 1 if any BOLTED or CONDEMNED verdict
fired — so MO is usable as a CI gate.
"""
from __future__ import annotations

import importlib.util
import json
import sys
import time

from .engine import RunResult, iter_verdicts, run
from .report import human_summary, jsonl_lines, verdict_to_dict
from .rules import Spec
from .trace import read_jsonl


def load_spec(path: str) -> Spec:
    if path.endswith(".zspec"):
        from .parser import parse_file
        return parse_file(path)

    # .py fallback: a module that defines a module-level `spec`
    spec_module = importlib.util.spec_from_file_location("_mo_spec", path)
    if spec_module is None or spec_module.loader is None:
        raise ValueError(f"cannot load spec from {path}")
    module = importlib.util.module_from_spec(spec_module)
    spec_module.loader.exec_module(module)
    return module.spec


def watch(spec: Spec, path: str, *, tick_ms: float = 100.0,
          idle_ms: float | None = None, clock=None, sleep=time.sleep,
          stop=None, out=None) -> RunResult:
    """Judge a growing glassport session log in real time.

    Tails `path`, projects each new frame to ZeusEvents in wire order, and
    interleaves synthetic ticks so liveness windows expire during silence
    (premortem #1). Verdicts stream to `out` (default stdout) the instant they
    fire, not at session end — that is what "judges a live server" means. The
    protocol projection and the wall clock both live in the adapters, never in
    the engine, so determinism and the A2A seam hold (premortems #4, #6).
    """
    from .adapters.live import tail_events
    from .adapters.mcp import frame_projector

    out = sys.stdout if out is None else out
    kw = {"tick_ms": tick_ms, "idle_ms": idle_ms, "sleep": sleep}
    if clock is not None:
        kw["clock"] = clock
    if stop is not None:
        kw["stop"] = stop

    events = tail_events(path, project=frame_projector(), **kw)
    result = RunResult()
    for v in iter_verdicts(events, spec, stats=result):
        result.verdicts.append(v)
        print(json.dumps(verdict_to_dict(v)), file=out, flush=True)
    print(human_summary(result), file=sys.stderr)
    return result


def _eval(spec_path: str, trace_path: str, mcp: bool) -> int:
    spec = load_spec(spec_path)
    if mcp:
        # --mcp: the file is a Glassport tap session, not a ZeusEvent log.
        # Lazy import keeps the protocol lib out of MO's core import graph.
        from .adapters.mcp import from_mcp_session_file
        events = from_mcp_session_file(trace_path)
    else:
        events = read_jsonl(trace_path)
    result = run(events, spec)

    for line in jsonl_lines(result):
        print(line)
    print(human_summary(result), file=sys.stderr)

    deviations = sum(1 for v in result.verdicts if v.kind in ("BOLTED", "CONDEMNED"))
    return 1 if deviations else 0


def _watch(spec_path: str, session_path: str, idle_ms: float | None) -> int:
    spec = load_spec(spec_path)
    result = watch(spec, session_path, idle_ms=idle_ms)
    deviations = sum(1 for v in result.verdicts if v.kind in ("BOLTED", "CONDEMNED"))
    return 1 if deviations else 0


_USAGE = (
    "usage:\n"
    "  mo eval  [--mcp] <spec.zspec|spec.py> <trace.jsonl>\n"
    "  mo watch [--idle-ms N] <spec.zspec|spec.py> <session.jsonl>"
)


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv:
        print(_USAGE, file=sys.stderr)
        return 2
    cmd, rest = argv[0], argv[1:]

    if cmd == "eval":
        mcp = "--mcp" in rest
        rest = [a for a in rest if a != "--mcp"]
        if len(rest) != 2:
            print(_USAGE, file=sys.stderr)
            return 2
        return _eval(rest[0], rest[1], mcp)

    if cmd == "watch":
        idle_ms: float | None = None
        if "--idle-ms" in rest:
            i = rest.index("--idle-ms")
            idle_ms = float(rest[i + 1])
            rest = rest[:i] + rest[i + 2:]
        if len(rest) != 2:
            print(_USAGE, file=sys.stderr)
            return 2
        return _watch(rest[0], rest[1], idle_ms)

    print(_USAGE, file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
