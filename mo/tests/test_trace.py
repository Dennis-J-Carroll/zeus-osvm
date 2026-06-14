from mo.trace import read_jsonl
from mo.events import ZeusEvent


def test_reads_jsonl_into_zeus_events(tmp_path):
    f = tmp_path / "trace.jsonl"
    f.write_text(
        '{"primitive": "tool_declared", "identifier": "search", "ts": 0.0}\n'
        '{"primitive": "tool_called", "identifier": "search", '
        '"payload": {"args": 1}, "ts": 1000.0}\n'
    )

    events = list(read_jsonl(str(f)))

    assert events == [
        ZeusEvent("tool_declared", "search", {}, 0.0),
        ZeusEvent("tool_called", "search", {"args": 1}, 1000.0),
    ]


def test_blank_lines_are_skipped(tmp_path):
    f = tmp_path / "trace.jsonl"
    f.write_text('{"primitive": "log", "identifier": null, "ts": 5.0}\n\n')

    events = list(read_jsonl(str(f)))

    assert len(events) == 1
    assert events[0].identifier is None
