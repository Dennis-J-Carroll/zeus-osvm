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


DEFAULT_TICK_MS = 100.0     # ceiling: ordinary multi-second liveness windows
DEFAULT_POLL_S = 0.05       # historical poll; auto_poll_s shrinks it with the tick
MIN_TICK_MS = 5.0           # floor: a tick finer than this buys nothing
MIN_POLL_S = 0.001


def _wall_ms() -> float:
    return time.time() * 1000.0


def _never() -> bool:
    return False


def auto_tick_ms(spec) -> float:
    """The live tick interval a spec needs (the §8 granularity decision).

    A BOLT can be noticed at most one effective tick after its window boundary,
    so the tick interval is the notice-lag budget. Keep the historical 100ms
    ceiling for ordinary multi-second windows, but tighten to one tenth of the
    tightest declared window: a fixed 100ms tick is a whole window late for a
    50ms obligation. The floor keeps a requested tick observable on the poll
    grid instead of spinning below it.
    """
    windows = [t.window_ms for t in spec.triggers
               if t.window_ms is not None and t.window_ms > 0]
    if not windows:
        return DEFAULT_TICK_MS
    return max(MIN_TICK_MS, min(DEFAULT_TICK_MS, min(windows) / 10.0))


def auto_poll_s(tick_ms: float) -> float:
    """Poll at half the tick interval (capped at 50ms) so ticks land on time.

    A ``tick_ms`` shorter than the poll period is invisible: the tailer looks
    at the clock only once per poll. Shrinking the poll with the tick keeps the
    effective period equal to the requested tick.
    """
    return min(DEFAULT_POLL_S, max(MIN_POLL_S, tick_ms / 1000.0 / 2.0))


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
    An idle return emits one final tick at the cutoff, so a window that lapsed
    during the last quiet interval is reported as a lapse rather than as an
    end-of-session BOLT.

    Malformed JSON lines are skipped: a crashing server may write a truncated
    frame, but the tailer must stay alive to judge whatever follows.
    """
    last_tick = last_line = clock()
    with open(path, encoding="utf-8") as fh:
        while not stop():
            pos = fh.tell()
            line = fh.readline()
            if line.endswith("\n"):
                last_line = clock()
                try:
                    rec = json.loads(line)
                except json.JSONDecodeError:
                    # truncated/corrupt line mid-write — leave it behind and
                    # continue tailing so later complete frames are still judged.
                    continue
                yield from project(rec)
                continue
            # no complete line right now — rewind over any partial read
            fh.seek(pos)
            now = clock()
            if idle_ms is not None and now - last_line >= idle_ms:
                if now > last_tick:
                    # One last time-carrier at the cutoff. A window that
                    # lapsed during the final quiet interval must be noticed
                    # as a lapse (`liveness_window_expired` with an exact
                    # `lapsed_at`), not as `session_ended_unresolved`. With a
                    # coarse tick the first due tick can be later than the
                    # idle return, so waiting for it would miss the lapse.
                    yield ZeusEvent("tick", None, ts=now)
                return
            if now - last_tick >= tick_ms:
                last_tick = now
                yield ZeusEvent("tick", None, ts=now)
            sleep(poll_s)
