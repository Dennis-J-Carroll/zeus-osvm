import json

from mo.engine import RunResult, Verdict
from mo.report import verdict_to_dict, human_summary, render_html


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


def _three_kinds():
    return RunResult(
        verdicts=[
            Verdict("FULFILLED", "web_search", 6, 0.0, 1200.0),
            Verdict("BOLTED", "db_query", 6, 2000.0, 7001.0,
                    {"reason": "liveness_window_expired", "window_ms": 5000}),
            Verdict("CONDEMNED", "arxiv_lookup", 9, None, 3000.0,
                    {"rule": "no fabricated calls"}),
        ],
        total_events=5,
        unmatched_events=1,
    )


def test_render_html_is_a_self_contained_offline_document():
    page = render_html(_three_kinds(), title="demo session")

    assert page.lstrip().startswith("<!DOCTYPE html>")
    assert "</html>" in page
    # opens offline from file:// — no external fetches, no script
    assert "http://" not in page and "https://" not in page
    assert "<script" not in page.lower()
    # every verdict and kind surfaces
    for ident in ("web_search", "db_query", "arxiv_lookup"):
        assert ident in page
    for kind in ("FULFILLED", "BOLTED", "CONDEMNED"):
        assert kind in page
    # the coverage signal (premortem #5) is on the page, not just the CLI
    assert "coverage" in page.lower()
    # a timeline: obligations are positioned spans (percent geometry)
    assert "width:" in page and "%" in page


def test_render_html_escapes_hostile_identifiers():
    # a server can name a tool '<img onerror=...>'; the report opens from
    # file:// where injected script runs with local reach. Must render as text.
    hostile = "<img src=x onerror=alert(1)>"
    page = render_html(RunResult(
        verdicts=[Verdict("CONDEMNED", hostile, 9, None, 1.0)]))

    assert hostile not in page
    assert "&lt;img" in page


def test_render_html_banner_flips_on_deviations():
    clean = render_html(RunResult(
        verdicts=[Verdict("FULFILLED", "a", 1, 0.0, 1.0)],
        total_events=1, unmatched_events=0))
    dirty = render_html(_three_kinds())

    assert "CLEAN" in clean.upper()
    assert "CLEAN" not in dirty.upper()
