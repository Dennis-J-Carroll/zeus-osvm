"""The live tailer (M3): follow a growing JSONL and inject ticks during
silence. Protocol-agnostic — projection is injected, so these tests never
touch MCP or glassport.
"""
import json
import sys

from mo.events import ZeusEvent


def test_tail_emits_projected_events_then_ticks_during_silence(tmp_path):
    from mo.adapters.live import tail_events

    p = tmp_path / "s.jsonl"
    p.write_text(json.dumps({"x": 1}) + "\n")

    clocks = iter([0.0, 1000.0])         # jumps past tick_ms once at EOF
    stops = iter([False, False, True])   # one read, one tick, then stop

    def project(rec):
        yield ZeusEvent("tool_called", "s", ts=0.0)

    evs = list(tail_events(
        str(p), project=project, tick_ms=100,
        clock=lambda: next(clocks, 1000.0),
        sleep=lambda *_: None,
        stop=lambda: next(stops, True),
    ))

    assert [e.primitive for e in evs] == ["tool_called", "tick"]
    tick = evs[1]
    assert tick.identifier is None
    assert tick.ts == 1000.0   # tick rides the wall clock, same scale as frames


def test_tail_does_not_tick_before_the_interval_elapses(tmp_path):
    from mo.adapters.live import tail_events

    p = tmp_path / "s.jsonl"
    p.write_text("")            # nothing to read; pure silence

    clocks = iter([0.0, 50.0, 90.0])   # never reaches tick_ms=100
    stops = iter([False, False, False, True])

    evs = list(tail_events(
        str(p), project=lambda rec: iter(()), tick_ms=100,
        clock=lambda: next(clocks, 90.0),
        sleep=lambda *_: None,
        stop=lambda: next(stops, True),
    ))

    assert evs == []   # no event crossed and no interval elapsed -> no tick


def test_tail_terminates_after_idle_timeout(tmp_path):
    # a live session ends when the tapped process goes quiet for good. idle_ms
    # lets the tail return on its own instead of looping forever.
    from mo.adapters.live import tail_events

    p = tmp_path / "s.jsonl"
    p.write_text(json.dumps({"x": 1}) + "\n")

    # init=0, read@100, poll@150 (tick), poll@350 (idle>=200 -> return)
    clocks = iter([0.0, 100.0, 150.0, 350.0])

    def project(rec):
        yield ZeusEvent("tool_called", "s", ts=0.0)

    evs = list(tail_events(
        str(p), project=project, tick_ms=100, idle_ms=200,
        clock=lambda: next(clocks, 999.0), sleep=lambda *_: None,
    ))

    assert [e.primitive for e in evs] == ["tool_called", "tick"]


def test_live_tailer_is_protocol_agnostic():
    # the seam: the generic tailer must not drag in glassport or the MCP adapter
    import mo.adapters.live  # noqa: F401
    assert "glassport" not in sys.modules


def test_tail_skips_truncated_jsonl_line(tmp_path):
    # a crashing server can leave a partial line in the tap log; the tailer must
    # not die, and must still process the next complete line.
    from mo.adapters.live import tail_events

    p = tmp_path / "s.jsonl"
    p.write_text('{"x": 1' + '\n{"x": 2}\n')   # first line is truncated

    clocks = iter([0.0, 0.0, 0.0])
    stops = iter([False, False, False, True])

    def project(rec):
        yield ZeusEvent("log", str(rec.get("x")), ts=0.0)

    evs = list(tail_events(
        str(p), project=project, tick_ms=1000,
        clock=lambda: next(clocks, 0.0),
        sleep=lambda *_: None,
        stop=lambda: next(stops, True),
    ))

    assert [e.identifier for e in evs] == ["2"]
