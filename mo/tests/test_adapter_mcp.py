import os
import types

import pytest

from mo.adapters.mcp import trace_to_events, frames_to_events, frame_projector
from mo.engine import run
from mo.parser import parse

EXAMPLES = os.path.join(os.path.dirname(__file__), "..", "examples")
SESSION = os.path.join(EXAMPLES, "mcp_session.jsonl")
MCP_SPEC = open(os.path.join(EXAMPLES, "mcp_liveness.zspec")).read()


# --- pure projection: no glassport needed (the seam) ---------------------

def _part(kind, content):
    return types.SimpleNamespace(kind=kind, content=content)


def _evt(kind, ts, parts=(), metadata=None):
    return types.SimpleNamespace(kind=kind, timestamp=ts,
                                 parts=list(parts), metadata=metadata or {})


class _FakeTrace:
    def __init__(self, declared, events):
        self._declared = declared
        self.events = events

    def declared_tools(self):
        return self._declared


def test_trace_to_events_projects_declarations_calls_results():
    trace = _FakeTrace(
        declared={"web_search"},
        events=[
            _evt("tool_call", "2026-06-09T18:39:29.496545+00:00",
                 parts=[_part("tool_use",
                              {"name": "web_search", "arguments": {"q": "x"}})]),
            _evt("tool_result", "2026-06-09T18:39:29.547143+00:00",
                 parts=[_part("tool_result", {"is_error": False, "output": "ok"})],
                 metadata={"tool_name": "web_search"}),
        ],
    )

    evs = list(trace_to_events(trace))
    prims = [(e.primitive, e.identifier) for e in evs]

    assert ("tool_declared", "web_search") in prims
    assert ("tool_called", "web_search") in prims
    assert ("tool_result", "web_search") in prims
    # a declaration must be visible before the call that references it
    assert (prims.index(("tool_declared", "web_search"))
            < prims.index(("tool_called", "web_search")))
    # timestamps are monotonic non-decreasing (engine relies on ev.ts)
    ts = [e.ts for e in evs]
    assert ts == sorted(ts)
    # call payload is carried through for ASSERT predicates
    call = next(e for e in evs if e.primitive == "tool_called")
    assert call.payload["arguments"] == {"q": "x"}


def test_trace_to_events_needs_no_glassport_import():
    import sys
    # building events from a duck-typed trace must not pull in the protocol lib
    trace = _FakeTrace(declared=set(), events=[])
    list(trace_to_events(trace))
    assert "glassport" not in sys.modules


# --- streaming projection: glassport JSONL records -> events in wire order ---

def test_frames_to_events_projects_in_wire_order():
    # live mode sees frames one at a time and cannot pre-scan declarations the
    # way the batch trace path does: events come out in the order they crossed.
    records = [
        {"dir": "c2s", "ts": "2026-06-09T18:39:29.000000+00:00",
         "frame": {"id": 1, "method": "initialize"}},
        {"dir": "s2c", "ts": "2026-06-09T18:39:29.100000+00:00",
         "frame": {"id": 2, "result": {"tools": [{"name": "web_search"}]}}},
        {"dir": "c2s", "ts": "2026-06-09T18:39:29.200000+00:00",
         "frame": {"id": 3, "method": "tools/call",
                   "params": {"name": "web_search", "arguments": {"q": "x"}}}},
        {"dir": "s2c", "ts": "2026-06-09T18:39:29.300000+00:00",
         "frame": {"id": 3, "result": {"content": [{"type": "text"}]}}},
    ]

    evs = list(frames_to_events(records))
    prims = [(e.primitive, e.identifier) for e in evs]

    # declaration emitted when tools/list response crosses, not up front
    assert prims.index(("tool_declared", "web_search")) \
        < prims.index(("tool_called", "web_search"))
    assert ("tool_called", "web_search") in prims
    # the call response is correlated back to its tool name by request id
    assert ("tool_result", "web_search") in prims
    # timestamps are monotonic non-decreasing (engine relies on ev.ts)
    ts = [e.ts for e in evs]
    assert ts == sorted(ts)
    # nothing is silently dropped — the initialize frame still surfaces
    assert any(e.identifier is None and e.primitive not in
               ("tool_declared", "tool_called", "tool_result") for e in evs)


def test_frames_to_events_flags_result_errors():
    records = [
        {"dir": "c2s", "ts": "2026-06-09T18:39:29.200000+00:00",
         "frame": {"id": 7, "method": "tools/call",
                   "params": {"name": "flaky", "arguments": {}}}},
        {"dir": "s2c", "ts": "2026-06-09T18:39:29.300000+00:00",
         "frame": {"id": 7, "result": {"isError": True, "content": []}}},
    ]

    evs = list(frames_to_events(records))
    result = next(e for e in evs if e.primitive == "tool_result")

    assert result.identifier == "flaky"
    assert result.payload["is_error"] is True


def test_frame_projector_correlates_results_across_separate_records():
    # the live tailer hands the projector ONE record at a time, so the call->
    # result id correlation must survive across calls, not reset each line.
    project = frame_projector()
    call = {"dir": "c2s", "ts": "2026-06-09T18:39:29.200000+00:00",
            "frame": {"id": 9, "method": "tools/call",
                      "params": {"name": "w", "arguments": {}}}}
    resp = {"dir": "s2c", "ts": "2026-06-09T18:39:29.300000+00:00",
            "frame": {"id": 9, "result": {"content": []}}}

    evs = list(project(call)) + list(project(resp))
    prims = [(e.primitive, e.identifier) for e in evs]

    assert prims == [("tool_called", "w"), ("tool_result", "w")]


# --- end-to-end through the real Glassport ingest ------------------------

def test_real_mcp_session_through_mo_yields_expected_verdicts():
    pytest.importorskip("glassport")
    from mo.adapters.mcp import from_mcp_session_file

    result = run(from_mcp_session_file(SESSION), parse(MCP_SPEC))
    kinds = [v.kind for v in result.verdicts]

    # web_search + arxiv_lookup each get a result -> 2 FULFILLED
    assert kinds.count("FULFILLED") == 2
    # arxiv_lookup was never declared -> exactly one CONDEMNED, on it
    assert kinds.count("CONDEMNED") == 1
    assert kinds.count("BOLTED") == 0
    condemned = next(v for v in result.verdicts if v.kind == "CONDEMNED")
    assert condemned.identifier == "arxiv_lookup"


def test_frame_projector_keeps_concurrent_same_tool_calls_separate():
    # premortem #2 live variant: two calls to the same tool, results returned
    # in reverse order. Correlation by jsonrpc id must pair each result with
    # its own call, not the first still-open call.
    project = frame_projector()
    records = [
        {"dir": "c2s", "ts": "2026-06-09T18:39:29.100000+00:00",
         "frame": {"id": 1, "method": "tools/call",
                   "params": {"name": "search", "arguments": {"q": "a"}}}},
        {"dir": "c2s", "ts": "2026-06-09T18:39:29.200000+00:00",
         "frame": {"id": 2, "method": "tools/call",
                   "params": {"name": "search", "arguments": {"q": "b"}}}},
        # result for id 2 arrives BEFORE result for id 1
        {"dir": "s2c", "ts": "2026-06-09T18:39:29.300000+00:00",
         "frame": {"id": 2, "result": {"content": []}}},
        {"dir": "s2c", "ts": "2026-06-09T18:39:29.400000+00:00",
         "frame": {"id": 1, "result": {"content": []}}},
    ]

    evs = list(frames_to_events(records))
    results = [(e.primitive, e.identifier, e.correlation) for e in evs]

    assert results == [
        ("tool_called", "search", "1"),
        ("tool_called", "search", "2"),
        ("tool_result", "search", "2"),
        ("tool_result", "search", "1"),
    ]


def test_frame_projector_survives_malformed_tool_call():
    # missing params, missing name, missing id — the projector must not crash
    # and should still surface something for coverage.
    records = [
        {"dir": "c2s", "ts": "2026-06-09T18:39:29.100000+00:00",
         "frame": {"id": 1, "method": "tools/call"}},   # no params
        {"dir": "c2s", "ts": "2026-06-09T18:39:29.200000+00:00",
         "frame": {"id": 2, "method": "tools/call",
                   "params": {"arguments": {}}}},        # no name
        {"dir": "c2s", "ts": "2026-06-09T18:39:29.300000+00:00",
         "frame": {"method": "tools/call",
                   "params": {"name": "x"}}},            # no id
    ]

    evs = list(frames_to_events(records))

    assert all(e.primitive == "tool_called" for e in evs)
    assert [e.identifier for e in evs] == [None, None, "x"]
    assert [e.correlation for e in evs] == ["1", "2", None]


def test_frame_projector_survives_result_without_matching_call():
    # a result with an unknown id is not correlated; it surfaces as a generic
    # frame so the session is still judged rather than silently dropped.
    records = [
        {"dir": "s2c", "ts": "2026-06-09T18:39:29.300000+00:00",
         "frame": {"id": 99, "result": {"content": []}}},
    ]

    evs = list(frames_to_events(records))

    assert len(evs) == 1
    assert evs[0].primitive == "response"
    assert evs[0].identifier is None
