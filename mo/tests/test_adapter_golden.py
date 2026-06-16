"""Golden contract test for the Glassport -> ZeusEvent seam (premortem #4/#5).

MO's core never imports glassport, so nothing in the rest of the suite would
notice if Glassport's wire/trace shape drifted. These tests freeze the contract:
a real-shaped tap log in, an exact ZeusEvent stream out. If Glassport changes
its frame shape, this is the test that goes red — deliberately.

No `glassport` import here: we exercise the PURE projection layer
(`frames_to_events`, `trace_to_events`) against frozen fixtures. The seam test
(test_seam.py) guarantees that purity; this test guarantees the projection is
*correct*, not merely import-clean.
"""
import json
import os
import types

from mo.adapters.mcp import trace_to_events
from mo.engine import run
from mo.parser import parse
from mo.tests.fixtures.regen import project_golden, project_trace_golden

HERE = os.path.dirname(__file__)
FIXTURES = os.path.join(HERE, "fixtures")
GOLDEN_EVENTS = os.path.join(FIXTURES, "mcp_session.events.json")
TRACE_EVENTS = os.path.join(FIXTURES, "trace_shape.events.json")
EXAMPLES = os.path.join(HERE, "..", "examples")
MCP_SPEC = open(os.path.join(EXAMPLES, "mcp_liveness.zspec")).read()


def _load_golden_events() -> list[dict]:
    with open(GOLDEN_EVENTS, encoding="utf-8") as fh:
        return json.load(fh)


def _load_trace_events() -> list[dict]:
    with open(TRACE_EVENTS, encoding="utf-8") as fh:
        return json.load(fh)


# --- the contract: wire-order projection matches the frozen golden ----------

def test_frame_projection_matches_golden():
    """The live frame projector's output is byte-for-byte the frozen contract.

    If this fails after a Glassport change, the adapter drifted. Inspect the
    diff, and only run `python3 -m mo.tests.fixtures.regen` if the new shape is
    intended.
    """
    produced = project_golden()
    expected = _load_golden_events()
    assert produced == expected, (
        "Adapter projection drifted from the golden contract. "
        "If intentional, regenerate with: python3 -m mo.tests.fixtures.regen"
    )


def test_golden_is_not_empty_and_is_well_formed():
    """Guard against a regen that silently produced nothing (premortem #5)."""
    events = _load_golden_events()
    assert events, "golden event stream is empty — regen likely broke"
    for ev in events:
        assert set(ev) == {"primitive", "identifier", "correlation",
                           "payload", "ts"}
        assert isinstance(ev["primitive"], str)


# --- the divergence the seam must preserve: batch hoists, wire-order can't ---

def test_batch_hoists_declarations_but_wire_order_does_not():
    """The §9 fabrication crime scene depends on this asymmetry.

    In the golden session `arxiv_lookup` is *called* (seq 4-5) before any tool
    is *declared* (seq 7). The wire-order projector therefore emits the
    declaration AFTER both calls — so a wire-order fabrication ASSERT cannot
    see it (the known M3 racy-membership limitation). The batch trace path
    hoists declarations to the front, so the ASSERT *can* catch it. This test
    pins both halves so neither path can quietly change behavior.
    """
    # wire-order: declaration lands after the calls
    wire = project_golden()
    prims = [(e["primitive"], e["identifier"]) for e in wire]
    decl_i = prims.index(("tool_declared", "web_search"))
    call_i = prims.index(("tool_called", "web_search"))
    assert decl_i > call_i, "wire-order should NOT hoist declarations"

    # batch: a trace that declares web_search hoists it before any call
    trace = _FakeTrace(
        declared={"web_search"},
        events=[
            _evt("tool_call", "2026-06-09T18:39:29.496545+00:00",
                 parts=[_part("tool_use",
                              {"name": "web_search", "arguments": {}})]),
        ],
    )
    batch = [(e.primitive, e.identifier) for e in trace_to_events(trace)]
    assert (batch.index(("tool_declared", "web_search"))
            < batch.index(("tool_called", "web_search"))), \
        "batch path MUST hoist declarations so the fabrication ASSERT sees them"


# --- end-to-end through the judge: the golden produces a real verdict --------

def test_trace_projection_matches_golden():
    """The trace path (the one that consumes a real Glassport InteractionTrace)
    is byte-for-byte the frozen contract. THIS is the test that catches a drift
    in metadata.jsonrpc_id, .parts shape, .declared_tools(), etc. — the surface
    the frame projector never touches.
    """
    produced = project_trace_golden()
    expected = _load_trace_events()
    assert produced == expected, (
        "trace_to_events drifted from the golden contract. "
        "If intentional, regenerate with: python3 -m mo.tests.fixtures.regen"
    )


def test_trace_path_catches_fabrication():
    """The batch trace path hoists declarations, so the fabrication ASSERT in
    mcp_liveness.zspec CONDEMNs arxiv_lookup (called, never declared). This is
    the §9 crime scene the wire-order path provably cannot catch (above)."""
    spec = parse(MCP_SPEC)
    events = [_dict_to_event(d) for d in _load_trace_events()]
    result = run(iter(events), spec)
    condemned = [v.identifier for v in result.verdicts if v.kind == "CONDEMNED"]
    assert "arxiv_lookup" in condemned
    assert "web_search" not in condemned


def test_wire_order_golden_two_fulfilled():
    """Feed the golden stream through MO. Both calls get results within the
    window -> two FULFILLED, zero BOLTED. (Wire-order can't CONDEMN the
    arxiv_lookup fabrication; that's the documented M3 limitation, asserted
    above, not a bug here.)"""
    spec = parse(MCP_SPEC)
    events = [_dict_to_event(d) for d in _load_golden_events()]
    result = run(iter(events), spec)
    kinds = [v.kind for v in result.verdicts]
    assert kinds.count("FULFILLED") == 2
    assert "BOLTED" not in kinds


# --- helpers ----------------------------------------------------------------

def _dict_to_event(d: dict):
    from mo.events import ZeusEvent
    return ZeusEvent(d["primitive"], d["identifier"],
                     correlation=d["correlation"],
                     payload=d["payload"], ts=d["ts"])


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
