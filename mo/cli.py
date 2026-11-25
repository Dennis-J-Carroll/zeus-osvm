"""MO command-line entry point.

    mo eval  <spec.zspec|spec.py> <trace.jsonl>   replay a recorded stream
    mo watch <spec.zspec|spec.py> <session.jsonl> judge a glassport tap log live
    mo watch --log-dir DIR <spec> -- <cmd...>    launch the tap and judge it in one command

`.zspec` files are parsed by the M1 text parser. `.py` files (M0 form) are
imported and must define a module-level `spec` (a rules.Spec). Both resolve
behind the same `load_spec` call.

Exit code: 0 if the session is clean, 1 if any BOLTED or CONDEMNED verdict
fired — so MO is usable as a CI gate.
"""
from __future__ import annotations

import importlib.util
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

from .engine import RunResult, iter_verdicts, run
from .parser import ParseError
from .report import human_summary, jsonl_lines, render_html, verdict_to_dict
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


def watch(spec: Spec, path: str, *, tick_ms: float | None = None,
          idle_ms: float | None = None, clock=None, sleep=time.sleep,
          stop=None, out=None, poll_s: float | None = None) -> RunResult:
    """Judge a growing glassport session log in real time.

    Tails `path`, projects each new frame to ZeusEvents in wire order, and
    interleaves synthetic ticks so liveness windows expire during silence
    (premortem #1). Verdicts stream to `out` (default stdout) the instant they
    fire, not at session end — that is what "judges a live server" means. The
    protocol projection and the wall clock both live in the adapters, never in
    the engine, so determinism and the A2A seam hold (premortems #4, #6).

    By default the tick interval is derived from the spec (100ms ceiling, one
    tenth of the tightest declared window, 5ms floor) and the poll follows it,
    so a short liveness window is noticed within roughly 10% of its length
    instead of one full default tick late. Pass `tick_ms` to override.
    """
    from .adapters.live import auto_poll_s, auto_tick_ms, tail_events
    from .adapters.mcp import frame_projector

    if tick_ms is None:
        tick_ms = auto_tick_ms(spec)
    if poll_s is None:
        poll_s = auto_poll_s(tick_ms)

    out = sys.stdout if out is None else out
    kw = {"tick_ms": tick_ms, "poll_s": poll_s, "idle_ms": idle_ms,
          "sleep": sleep}
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


def _watch(spec_path: str, session_path: str, idle_ms: float | None,
           tick_ms: float | None) -> int:
    spec = load_spec(spec_path)
    result = watch(spec, session_path, idle_ms=idle_ms, tick_ms=tick_ms)
    deviations = sum(1 for v in result.verdicts if v.kind in ("BOLTED", "CONDEMNED"))
    return 1 if deviations else 0


def _new_log(log_dir: str, before: set[str], wait_ms: float) -> str | None:
    """Wait for a tap session log that the launched process creates.

    Glassport names its session log after the child (`<stamp>_<name>_<pid>.jsonl`),
    so the path is not knowable before launch. Snapshot the directory, start the
    command, then take the newest new `*.jsonl` — exactly the retry loop the
    real-failure demo used by hand.
    """
    deadline = time.monotonic() + wait_ms / 1000.0
    directory = Path(log_dir)
    while True:
        candidates = [p for p in directory.glob("*.jsonl")
                      if str(p) not in before]
        if candidates:
            return str(max(candidates, key=lambda p: p.stat().st_mtime_ns))
        if time.monotonic() >= deadline:
            return None
        time.sleep(0.02)


def _stop_child(child: subprocess.Popen) -> None:
    """Terminate the launched process group, then wait for it."""
    if child.poll() is not None:
        child.wait()
        return
    try:
        os.killpg(os.getpgid(child.pid), signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        child.wait(timeout=2.0)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(os.getpgid(child.pid), signal.SIGKILL)
        except ProcessLookupError:
            pass
        child.wait()


def _watch_command(spec_path: str, command: list[str], log_dir: str,
                   idle_ms: float | None, tick_ms: float | None,
                   wait_ms: float) -> int:
    """`mo watch --log-dir DIR <spec> -- <command...>`: tap and judge together.

    The command is started in its own session (so abandoning the judge also
    stops its children), and its new session log is tailed as usual. When the
    command exits, a short grace poll drains the last records before the judge
    finalises; when the judge stops first, the command tree is terminated.
    """
    spec = load_spec(spec_path)
    if not Path(log_dir).is_dir():
        print(f"mo: --log-dir {log_dir} is not a directory", file=sys.stderr)
        return 2

    before = {str(p) for p in Path(log_dir).glob("*.jsonl")}
    print(f"# launched: {' '.join(command)}", file=sys.stderr)
    try:
        child = subprocess.Popen(command, start_new_session=True)
    except (OSError, ValueError) as exc:
        print(f"mo: cannot launch {command[0]}: {exc}", file=sys.stderr)
        return 2

    exited_at: float | None = None

    def stop() -> bool:
        # After the child exits, keep reading for a short grace window so its
        # final flushed records are judged; then stop and finalise.
        nonlocal exited_at
        if child.poll() is None:
            exited_at = None
            return False
        now = time.monotonic()
        if exited_at is None:
            exited_at = now
            return False
        return now - exited_at >= 0.2

    child_was_running = True
    try:
        print(f"# waiting for a new session log in {log_dir}", file=sys.stderr)
        log_path = _new_log(log_dir, before, wait_ms)
        if log_path is None:
            print(
                f"mo: no new *.jsonl appeared in {log_dir} within {wait_ms:.0f}ms",
                file=sys.stderr,
            )
            return 2
        print(f"# tap: {log_path}", file=sys.stderr)
        result = watch(spec, log_path, idle_ms=idle_ms, tick_ms=tick_ms,
                       stop=stop)
    except KeyboardInterrupt:
        print("mo: interrupted", file=sys.stderr)
        return 130
    finally:
        # Only a child that exited on its own can be judged as failing; a
        # termination MO performed because judging ended is not its failure.
        child_was_running = child.poll() is None
        _stop_child(child)

    deviations = sum(1 for v in result.verdicts if v.kind in ("BOLTED", "CONDEMNED"))
    child_failed = not child_was_running and child.returncode not in (0, None)
    if child_failed:
        print(f"mo: {command[0]} exited {child.returncode}", file=sys.stderr)
    return 1 if deviations or child_failed else 0


def _report(spec_path: str, trace_path: str, mcp: bool, out_path: str | None) -> int:
    spec = load_spec(spec_path)
    if mcp:
        from .adapters.mcp import from_mcp_session_file
        events = from_mcp_session_file(trace_path)
    else:
        events = read_jsonl(trace_path)
    result = run(events, spec)

    import os
    page = render_html(result, title=os.path.basename(trace_path))
    if out_path:
        with open(out_path, "w", encoding="utf-8") as fh:
            fh.write(page)
        print(f"wrote {out_path}", file=sys.stderr)
    else:
        print(page)
    print(human_summary(result), file=sys.stderr)

    deviations = sum(1 for v in result.verdicts if v.kind in ("BOLTED", "CONDEMNED"))
    return 1 if deviations else 0


_USAGE = (
    "usage:\n"
    "  mo eval   [--mcp] <spec.zspec|spec.py> <trace.jsonl>\n"
    "  mo watch  [--idle-ms N] [--tick-ms MS] <spec.zspec|spec.py> <session.jsonl>\n"
    "  mo watch  [--idle-ms N] [--tick-ms MS] [--wait-ms MS] --log-dir DIR\n"
    "            <spec.zspec|spec.py> -- <command...>\n"
    "  mo report [--mcp] <spec.zspec|spec.py> <trace.jsonl> [-o out.html]"
)


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv:
        print(_USAGE, file=sys.stderr)
        return 2
    cmd, rest = argv[0], argv[1:]

    try:
        if cmd == "eval":
            mcp = "--mcp" in rest
            rest = [a for a in rest if a != "--mcp"]
            if len(rest) != 2:
                print(_USAGE, file=sys.stderr)
                return 2
            return _eval(rest[0], rest[1], mcp)

        if cmd == "watch":
            idle_ms: float | None = None
            tick_ms: float | None = None
            wait_ms = 5000.0
            log_dir: str | None = None
            if "--idle-ms" in rest:
                i = rest.index("--idle-ms")
                idle_ms = float(rest[i + 1])
                rest = rest[:i] + rest[i + 2:]
            if "--tick-ms" in rest:
                i = rest.index("--tick-ms")
                tick_ms = float(rest[i + 1])
                rest = rest[:i] + rest[i + 2:]
            if "--wait-ms" in rest:
                i = rest.index("--wait-ms")
                wait_ms = float(rest[i + 1])
                rest = rest[:i] + rest[i + 2:]
            if "--log-dir" in rest:
                i = rest.index("--log-dir")
                log_dir = rest[i + 1]
                rest = rest[:i] + rest[i + 2:]

            if "--" in rest:
                i = rest.index("--")
                command = rest[i + 1:]
                spec_args = rest[:i]
                if len(spec_args) != 1 or not command:
                    print(_USAGE, file=sys.stderr)
                    return 2
                if log_dir is None:
                    print(
                        "mo: watch -- <cmd> needs --log-dir DIR so the new "
                        "session log can be found",
                        file=sys.stderr,
                    )
                    return 2
                return _watch_command(spec_args[0], command, log_dir,
                                      idle_ms, tick_ms, wait_ms)

            if log_dir is not None:
                # A log dir only means something when MO starts the process.
                print("mo: --log-dir requires a command after --",
                      file=sys.stderr)
                return 2
            if len(rest) != 2:
                print(_USAGE, file=sys.stderr)
                return 2
            return _watch(rest[0], rest[1], idle_ms, tick_ms)

        if cmd == "report":
            mcp = "--mcp" in rest
            rest = [a for a in rest if a != "--mcp"]
            out_path: str | None = None
            if "-o" in rest:
                i = rest.index("-o")
                out_path = rest[i + 1]
                rest = rest[:i] + rest[i + 2:]
            if len(rest) != 2:
                print(_USAGE, file=sys.stderr)
                return 2
            return _report(rest[0], rest[1], mcp, out_path)
    except FileNotFoundError as exc:
        print(f"mo: {exc}", file=sys.stderr)
        return 2
    except ParseError as exc:
        # .zspec text parser emits line N: errors; surface them cleanly.
        print(f"mo: {exc}", file=sys.stderr)
        return 2
    except ValueError as exc:
        # includes import failures from load_spec and bad numeric flags
        print(f"mo: {exc}", file=sys.stderr)
        return 2

    print(_USAGE, file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
