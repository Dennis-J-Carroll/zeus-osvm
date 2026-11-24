import io
import json
import os

from mo.cli import load_spec, main, watch

_EX = os.path.join(os.path.dirname(__file__), "..", "examples")


SPEC_SRC = '''
from mo.rules import Spec, TriggerRule

spec = Spec(triggers=[
    TriggerRule(on_primitive="tool_declared", expects="tool_called",
                window_ms=5000, spec_line=1),
])
'''


def _write_spec(tmp_path):
    p = tmp_path / "spec.py"
    p.write_text(SPEC_SRC)
    return p


ZSPEC_SRC = '''\
on tool_declared as $tool:
    EXPECT tool_called for $tool
    WINDOW 5000

ASSERT no tool_called where identifier not in tool_declared
'''

GRACE_SPEC_SRC = '''\
ASSERT no tool_called where identifier not in tool_declared GRACE 500
'''


def test_load_spec_imports_module_level_spec(tmp_path):
    spec = load_spec(str(_write_spec(tmp_path)))
    assert len(spec.triggers) == 1
    assert spec.triggers[0].expects == "tool_called"


def test_load_spec_parses_zspec_text(tmp_path):
    p = tmp_path / "spec.zspec"
    p.write_text(ZSPEC_SRC)
    spec = load_spec(str(p))
    assert len(spec.triggers) == 1
    assert len(spec.assertions) == 1
    assert spec.triggers[0].window_ms == 5000


def test_eval_with_zspec_runs_end_to_end(tmp_path, capsys):
    spec = tmp_path / "spec.zspec"
    spec.write_text(ZSPEC_SRC)
    trace = tmp_path / "trace.jsonl"
    trace.write_text('{"primitive":"tool_declared","identifier":"search","ts":0.0}\n')

    code = main(["eval", str(spec), str(trace)])
    out = capsys.readouterr().out

    assert code == 1
    assert '"kind": "BOLTED"' in out


def test_eval_grace_window_tolerates_late_declaration(tmp_path, capsys):
    spec = tmp_path / "grace.zspec"
    spec.write_text(GRACE_SPEC_SRC)
    trace = tmp_path / "trace.jsonl"
    trace.write_text(
        '{"primitive":"tool_called","identifier":"search","ts":0.0}\n'
        '{"primitive":"tool_declared","identifier":"search","ts":200.0}\n'
    )

    code = main(["eval", str(spec), str(trace)])
    out = capsys.readouterr().out

    assert code == 0
    assert "CONDEMNED" not in out


def test_eval_grace_window_condemns_after_window(tmp_path, capsys):
    spec = tmp_path / "grace.zspec"
    spec.write_text(GRACE_SPEC_SRC)
    trace = tmp_path / "trace.jsonl"
    trace.write_text(
        '{"primitive":"tool_called","identifier":"search","ts":0.0}\n'
        '{"primitive":"tool_declared","identifier":"search","ts":800.0}\n'
    )

    code = main(["eval", str(spec), str(trace)])
    out = capsys.readouterr().out

    assert code == 1
    assert '"kind": "CONDEMNED"' in out
    assert "late_declaration_grace_expired" in out


def test_watch_grace_window_tolerates_wire_order_declaration(tmp_path):
    # The live projector emits the declaration when the tools/list response
    # crosses the wire. This call is before it, but the declaration lands
    # inside the 500ms grace window, so live mode must not condemn it.
    spec = tmp_path / "grace.zspec"
    spec.write_text(GRACE_SPEC_SRC)
    session = tmp_path / "s.jsonl"
    session.write_text(
        json.dumps({
            "dir": "c2s", "seq": 1,
            "frame": {"id": 1, "method": "tools/call",
                      "params": {"name": "search", "arguments": {}}},
        }) + "\n" +
        json.dumps({
            "dir": "s2c", "seq": 2,
            "frame": {"id": 2, "result": {"tools": [{"name": "search"}]}},
        }) + "\n"
    )

    clocks = iter([0.0, 0.0, 0.0, 1000.0])
    result = watch(
        load_spec(str(spec)), str(session),
        tick_ms=100, idle_ms=100,
        clock=lambda: next(clocks, 1000.0),
        sleep=lambda *_: None, out=io.StringIO(),
    )

    assert result.verdicts == []


def test_eval_mcp_flag_routes_session_through_adapter(capsys):
    import pytest
    pytest.importorskip("glassport")

    spec = os.path.join(_EX, "mcp_liveness.zspec")
    session = os.path.join(_EX, "mcp_session.jsonl")

    code = main(["eval", "--mcp", spec, session])
    out = capsys.readouterr().out

    assert code == 1   # the fabricated arxiv_lookup condemns the session
    condemned = [json.loads(l) for l in out.splitlines()
                 if l.startswith("{") and json.loads(l)["kind"] == "CONDEMNED"]
    assert len(condemned) == 1
    assert condemned[0]["identifier"] == "arxiv_lookup"


def test_watch_bolts_a_silent_unanswered_call_mid_session(tmp_path):
    # the M3 demo: a live session calls a tool and the server goes quiet. A
    # synthetic tick must cross the window and BOLT mid-silence — not wait for
    # end-of-stream. Clock is injected so the silence is deterministic.
    spec_path = tmp_path / "live.zspec"
    spec_path.write_text(
        "on tool_called as $t:\n"
        "    EXPECT tool_result for $t\n"
        "    WINDOW 5000\n"
    )
    session = tmp_path / "s.jsonl"
    session.write_text(json.dumps({
        "dir": "c2s", "seq": 1,
        "frame": {"id": 5, "method": "tools/call",
                  "params": {"name": "search", "arguments": {}}},
    }) + "\n")   # ...and then nothing. the server never answers.

    # init=0, read@0, poll@6000 (>5001 -> tick bolts), poll@200000 (idle stop)
    clocks = iter([0.0, 0.0, 6000.0, 200000.0])
    out = io.StringIO()

    result = watch(
        load_spec(str(spec_path)), str(session),
        tick_ms=100, idle_ms=100000,
        clock=lambda: next(clocks, 200000.0),
        sleep=lambda *_: None, out=out,
    )

    kinds = [v.kind for v in result.verdicts]
    assert kinds == ["BOLTED"]
    bolt = result.verdicts[0]
    assert bolt.identifier == "search"
    assert bolt.detail["reason"] == "liveness_window_expired"  # the tick, not EOS
    # the verdict was streamed to stdout the moment it fired
    printed = [json.loads(l) for l in out.getvalue().splitlines() if l.strip()]
    assert printed and printed[0]["kind"] == "BOLTED"


def test_watch_default_tick_tightens_for_a_short_window(tmp_path):
    # the §8 decision: a 50ms window must not wait on a fixed 100ms tick. With
    # no tick_ms argument, watch derives ~5ms ticks (and a 2.5ms poll) from the
    # spec, so the BOLT lands within 10% of the window after it lapses.
    spec_path = tmp_path / "short.zspec"
    spec_path.write_text(
        "on tool_called as $t:\n"
        "    EXPECT tool_result for $t\n"
        "    WINDOW 50\n"
    )
    session = tmp_path / "s.jsonl"
    session.write_text(json.dumps({
        "dir": "c2s", "seq": 1,
        "frame": {"id": 5, "method": "tools/call",
                  "params": {"name": "search", "arguments": {}}},
    }) + "\n")

    class Clock:
        def __init__(self, step_ms):
            self.t = 0.0
            self.step = step_ms

        def __call__(self):
            now = self.t
            self.t += self.step
            return now

    clock = Clock(2.5)
    result = watch(
        load_spec(str(spec_path)), str(session),
        clock=clock, sleep=lambda *_: None,
        stop=lambda: clock.t > 300.0, out=io.StringIO(),
    )

    bolts = [v for v in result.verdicts if v.kind == "BOLTED"]
    assert len(bolts) == 1
    assert bolts[0].detail["reason"] == "liveness_window_expired"
    assert bolts[0].detail["lapsed_at"] == bolts[0].opened_at + 50.0
    assert 0 < bolts[0].detail["notice_lag_ms"] <= 10.0


def test_watch_cli_accepts_a_tick_ms_override(tmp_path, capsys):
    spec = tmp_path / "live.zspec"
    spec.write_text(
        "on tool_declared as $t:\n"
        "    EXPECT tool_called for $t\n"
        "    WINDOW 5000\n"
    )
    session = tmp_path / "s.jsonl"
    session.write_text("")

    code = main([
        "watch", "--tick-ms", "25", "--idle-ms", "0",
        str(spec), str(session),
    ])

    assert code == 0


def test_report_writes_self_contained_html(tmp_path):
    spec = tmp_path / "live.zspec"
    spec.write_text(
        "on tool_declared as $t:\n"
        "    EXPECT tool_called for $t\n"
        "    WINDOW 5000\n"
    )
    trace = tmp_path / "trace.jsonl"
    trace.write_text(
        json.dumps({"primitive": "tool_declared", "identifier": "x",
                    "payload": {}, "ts": 0.0}) + "\n"
    )   # declared, never called -> end-of-stream BOLT -> a deviation
    out = tmp_path / "report.html"

    code = main(["report", str(spec), str(trace), "-o", str(out)])

    assert code == 1   # the unhonored declaration deviates
    page = out.read_text()
    assert page.lstrip().startswith("<!DOCTYPE html>")
    assert "BOLTED" in page and "DEVIATIONS FOUND" in page


def test_eval_clean_session_exits_zero(tmp_path, capsys):
    spec = _write_spec(tmp_path)
    trace = tmp_path / "trace.jsonl"
    trace.write_text(
        '{"primitive":"tool_declared","identifier":"search","ts":0.0}\n'
        '{"primitive":"tool_called","identifier":"search","ts":1000.0}\n'
    )

    code = main(["eval", str(spec), str(trace)])
    captured = capsys.readouterr()

    assert code == 0
    assert '"kind": "FULFILLED"' in captured.out   # verdicts -> stdout
    assert "BOLTED: 0" in captured.err             # summary -> stderr


def test_eval_bolted_session_exits_nonzero_with_one_bolt(tmp_path, capsys):
    # §9 demo: declares a tool, never honors it -> exactly one BOLT
    spec = _write_spec(tmp_path)
    trace = tmp_path / "trace.jsonl"
    trace.write_text('{"primitive":"tool_declared","identifier":"search","ts":0.0}\n')

    code = main(["eval", str(spec), str(trace)])
    out = capsys.readouterr().out

    bolts = [l for l in out.splitlines()
             if l.startswith("{") and json.loads(l)["kind"] == "BOLTED"]
    assert len(bolts) == 1
    assert json.loads(bolts[0])["identifier"] == "search"
    assert code == 1


def test_eval_prints_zspec_parse_error_cleanly(tmp_path, capsys):
    bad = tmp_path / "bad.zspec"
    bad.write_text("on tool_declared as $t\n    EXPECT tool_called for $t\n")
    trace = tmp_path / "trace.jsonl"
    trace.write_text('{"primitive":"tool_declared","identifier":"x","ts":0.0}\n')

    code = main(["eval", str(bad), str(trace)])
    err = capsys.readouterr().err

    assert code == 2
    assert "mo:" in err
    assert "line" in err.lower()


def test_watch_prints_zspec_parse_error_cleanly(tmp_path, capsys):
    bad = tmp_path / "bad.zspec"
    bad.write_text("ASSERT no tool_called where identifier not in\n")
    session = tmp_path / "s.jsonl"
    session.write_text("")

    code = main(["watch", str(bad), str(session)])
    err = capsys.readouterr().err

    assert code == 2
    assert "mo:" in err
    assert "line" in err.lower()


def test_report_prints_zspec_parse_error_cleanly(tmp_path, capsys):
    bad = tmp_path / "bad.zspec"
    bad.write_text("on tool_declared\n")
    trace = tmp_path / "trace.jsonl"
    trace.write_text('{"primitive":"tool_declared","identifier":"x","ts":0.0}\n')

    code = main(["report", str(bad), str(trace)])
    err = capsys.readouterr().err

    assert code == 2
    assert "mo:" in err
    assert "line" in err.lower()


def test_eval_reports_the_fragment_file_for_an_included_rule(tmp_path, capsys):
    (tmp_path / "frag.zspec").write_text(
        "on tool_declared as $t:\n"
        "    EXPECT tool_called for $t\n"
        "    WINDOW 5000\n"
    )
    root = tmp_path / "root.zspec"
    root.write_text("INCLUDE frag.zspec\n")
    trace = tmp_path / "trace.jsonl"
    trace.write_text(
        '{"primitive":"tool_declared","identifier":"search","ts":0.0}\n'
    )

    code = main(["eval", str(root), str(trace)])
    out = capsys.readouterr().out
    verdicts = [json.loads(line) for line in out.splitlines() if line.strip()]

    assert code == 1
    bolt = verdicts[0]
    assert bolt["spec_source"].endswith("frag.zspec")
    assert bolt["spec_line"] == 1
