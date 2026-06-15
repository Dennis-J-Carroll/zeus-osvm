import os
import types

import pytest

from mo.adapters.mcp import trace_to_events
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
