#!/usr/bin/env python3
"""Live dogfood: prove `mo watch` fires a BOLT mid-session, in one command.

    python3 -m mo.examples.live_dogfood

Turns the manual `kimi_look.md` run into a repeatable, self-contained demo with
NO glassport dependency. It:

  1. writes a Glassport-shaped tap log to a temp file, one frame at a time, on a
     background thread (simulating a server being tapped in real time);
  2. the scripted server declares `web_search`, calls it, answers it (FULFILLED),
     then calls a second tool and *goes silent* — the §9 hung-server crime scene;
  3. runs MO's live tailer over the growing file with a liveness spec and a
     short window, so a synthetic tick crosses the window during the silence and
     BOLTs the unanswered call (premortem #1, the silence problem);
  4. prints the verdict stream and asserts exactly one BOLT, on the right tool.

Determinism note: the demo uses real wall-clock time for the tail (that's the
point — it exercises the live path), so timings are generous. The *engine* still
only reads ev.ts; only the adapter reads the clock (premortem #6).
"""
from __future__ import annotations

import json
import os
import sys
import tempfile
import threading
import time

from mo.adapters.mcp import frame_projector
from mo.adapters.live import tail_events
from mo.engine import iter_verdicts
from mo.parser import parse
from mo.report import verdict_to_dict

# A liveness-only spec with a short window so the demo runs in ~2s, not 10.
SPEC_TEXT = """\
; live dogfood: every tool call must return a result within 1s
on tool_called as $tool:
    EXPECT tool_result for $tool
    WINDOW 1000
"""


def _frame(seq: int, direction: str, frame: dict) -> str:
    rec = {
        "schema_version": "0.1",
        "seq": seq,
        "ts": _iso_now(),
        "dir": direction,
        "frame": frame,
        "raw": None,
    }
    return json.dumps(rec) + "\n"


def _iso_now() -> str:
    from datetime import datetime, timezone
    return datetime.now(timezone.utc).isoformat()


def _scripted_server(path: str, done: threading.Event) -> None:
    """Append frames to `path` over time: a clean call, then a silent hang."""
    def append(line: str) -> None:
        with open(path, "a", encoding="utf-8") as fh:
            fh.write(line)
            fh.flush()

    # handshake + declaration
    append(_frame(1, "c2s", {"jsonrpc": "2.0", "id": 1, "method": "initialize",
                             "params": {}}))
    append(_frame(2, "s2c", {"jsonrpc": "2.0", "id": 2,
                             "result": {"tools": [{"name": "web_search"}]}}))
    time.sleep(0.1)

    # a clean call that IS answered -> FULFILLED
    append(_frame(3, "c2s", {"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                             "params": {"name": "web_search",
                                        "arguments": {"query": "acme q3"}}}))
    time.sleep(0.1)
    append(_frame(4, "s2c", {"jsonrpc": "2.0", "id": 3,
                             "result": {"isError": False}}))
    time.sleep(0.1)

    # a call that is NEVER answered -> the server goes silent -> BOLT
    append(_frame(5, "c2s", {"jsonrpc": "2.0", "id": 4, "method": "tools/call",
                             "params": {"name": "report_export",
                                        "arguments": {}}}))
    # deliberately no result. hold the file open-but-quiet so the tailer's
    # synthetic tick is what crosses the window.
    done.wait(timeout=3.0)


def main() -> int:
    spec = parse(SPEC_TEXT)
    tmp = tempfile.NamedTemporaryFile(
        mode="w", suffix=".jsonl", prefix="mo_dogfood_", delete=False)
    tmp.close()
    path = tmp.name

    done = threading.Event()
    writer = threading.Thread(target=_scripted_server, args=(path, done),
                              daemon=True)
    writer.start()

    # stop tailing once we've seen the session go idle past the window.
    started = time.time()

    def stop() -> bool:
        return time.time() - started > 3.0

    verdicts = []
    print(f"# tailing {path}", file=sys.stderr)
    stream = tail_events(path, frame_projector(), tick_ms=100.0,
                         idle_ms=1500.0, stop=stop)
    for verdict in iter_verdicts(stream, spec):
        verdicts.append(verdict)
        print(json.dumps(verdict_to_dict(verdict)))

    done.set()
    os.unlink(path)

    kinds = [v.kind for v in verdicts]
    bolted = [v for v in verdicts if v.kind == "BOLTED"]
    print(f"\n# verdicts: {kinds}", file=sys.stderr)

    ok = (kinds.count("FULFILLED") == 1
          and len(bolted) == 1
          and bolted[0].identifier == "report_export")
    if ok:
        print("# PASS: one FULFILLED (web_search), one BOLT (report_export), "
              "fired mid-session via synthetic tick.", file=sys.stderr)
        return 0
    print("# FAIL: expected exactly one FULFILLED and one BOLT on "
          "report_export.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
