import json

from mo.engine import RunResult, Verdict
from mo.report import verdict_to_dict, human_summary


def test_verdict_to_dict_roundtrips_to_json():
    v = Verdict(kind="BOLTED", identifier="tool_x", spec_line=14,
                opened_at=0.0, closed_at=5001.0,
                detail={"window_ms": 5000, "reason": "liveness_window_expired"})

    d = verdict_to_dict(v)

    assert json.loads(json.dumps(d)) == {
        "kind": "BOLTED", "identifier": "tool_x", "spec_line": 14,
        "opened_at": 0.0, "closed_at": 5001.0,
        "detail": {"window_ms": 5000, "reason": "liveness_window_expired"},
    }


def test_human_summary_counts_each_verdict_kind():
    result = RunResult(
        verdicts=[
            Verdict("FULFILLED", "a", 1, 0.0, 1.0),
            Verdict("BOLTED", "b", 1, 0.0, 9.0, {"reason": "x"}),
            Verdict("CONDEMNED", "c", 3, None, 2.0),
        ],
        total_events=5,
        unmatched_events=2,
    )

    text = human_summary(result)

    assert "FULFILLED: 1" in text
    assert "BOLTED: 1" in text
    assert "CONDEMNED: 1" in text
    # coverage signal surfaced (premortem #5)
    assert "2/5" in text


def test_clean_session_summary_reports_no_deviations():
    result = RunResult(verdicts=[Verdict("FULFILLED", "a", 1, 0.0, 1.0)],
                       total_events=2, unmatched_events=0)

    text = human_summary(result)

    assert "BOLTED: 0" in text
    assert "CONDEMNED: 0" in text
