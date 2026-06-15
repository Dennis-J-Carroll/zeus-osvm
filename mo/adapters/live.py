"""Live mode (M3): follow a growing JSONL session log and feed MO in real
time, minting a synthetic `tick` during silence so liveness windows can
expire even when the agent goes quiet (premortem #1, the silence problem).

This module is deliberately protocol-agnostic. It knows how to *tail* a file
and *when* to tick; it does NOT know MCP. The record->ZeusEvent projection is
injected by the caller, so glassport never enters MO's core import graph and
the A2A-portability seam (premortem #4) holds.

The tick is the one place a wall clock is read — and it lives HERE, in the
adapter, never in the engine (premortem #6). The tick's timestamp is on the
same scale as the frame timestamps (epoch ms) so the engine's `ev.ts >
expires_at` comparison is meaningful. clock/sleep/stop are injectable so the
tick logic stays deterministically testable without real time passing.
"""
from __future__ import annotations

import json
import time
from typing import Callable, Iterator

from ..events import ZeusEvent


def _wall_ms() -> float:
    return time.time() * 1000.0


def _never() -> bool:
    return False


def tail_events(
    path: str,
    project: Callable[[dict], Iterator[ZeusEvent]],
    tick_ms: float = 100.0,
    idle_ms: float | None = None,
    clock: Callable[[], float] = _wall_ms,
    sleep: Callable[[float], None] = time.sleep,
    stop: Callable[[], bool] = _never,
    poll_s: float = 0.05,
) -> Iterator[ZeusEvent]:
    """Yield ZeusEvents from a JSONL file as it grows, plus ticks during silence.

    Each complete `\\n`-terminated line is parsed and handed to `project`,
    whose events are forwarded in order. A partial trailing line (writer hasn't
    flushed the newline yet) is left for the next poll. Between lines, if at
    least `tick_ms` has elapsed on `clock`, one tick is emitted. The loop ends
    when `stop()` returns True or — if `idle_ms` is set — when that much time
    has passed since the last line was read (the tapped process went quiet).
    """
    last_tick = last_line = clock()
    with open(path, encoding="utf-8") as fh:
        while not stop():
            pos = fh.tell()
            line = fh.readline()
            if line.endswith("\n"):
                last_line = clock()
                yield from project(json.loads(line))
                continue
            # no complete line right now — rewind over any partial read
            fh.seek(pos)
            now = clock()
            if idle_ms is not None and now - last_line >= idle_ms:
                return
            if now - last_tick >= tick_ms:
                last_tick = now
                yield ZeusEvent("tick", None, ts=now)
            sleep(poll_s)
