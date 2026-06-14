import json

from mo.cli import load_spec, main


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
