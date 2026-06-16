"""Regenerate the golden ZeusEvent stream from the frozen session input.

Run deliberately, never automatically:

    python3 -m mo.tests.fixtures.regen

It reads `mcp_session.golden.jsonl` (a real-shaped Glassport tap log), runs the
*wire-order* live projector over it, and writes the resulting ZeusEvent stream
to `mcp_session.events.json`. That file is the adapter contract the golden test
pins. Review the git diff after running: an unexpected change means the adapter
drifted (premortem #4/#5), which is exactly what the test exists to catch.

Note we serialize the FRAME-PROJECTOR (wire-order) output, not trace_to_events.
trace_to_events needs a live Glassport `InteractionTrace` object to hoist
declarations up front; the frame projector works on the raw JSONL records with
no glassport import, so the contract test stays seam-clean by construction.
"""
from __future__ import annotations

import json
import os
import types

from mo.adapters.mcp import frames_to_events, trace_to_events

HERE = os.path.dirname(__file__)
GOLDEN_INPUT = os.path.join(HERE, "mcp_session.golden.jsonl")
GOLDEN_EVENTS = os.path.join(HERE, "mcp_session.events.json")
TRACE_SHAPE = os.path.join(HERE, "trace_shape.json")
TRACE_EVENTS = os.path.join(HERE, "trace_shape.events.json")


def event_to_dict(ev) -> dict:
    """Stable, diff-friendly serialization of a ZeusEvent."""
    return {
        "primitive": ev.primitive,
        "identifier": ev.identifier,
        "correlation": ev.correlation,
        "payload": ev.payload,
        "ts": ev.ts,
    }


def project_golden() -> list[dict]:
    with open(GOLDEN_INPUT, encoding="utf-8") as fh:
        records = [json.loads(line) for line in fh if line.strip()]
    return [event_to_dict(ev) for ev in frames_to_events(records)]


def hydrate_trace(shape: dict):
    """Turn the frozen trace_shape.json dict into a duck-typed object with the
    same surface trace_to_events reads: .declared_tools(), .events[], and on
    each event .kind/.timestamp/.metadata/.parts[].kind/.parts[].content.

    This is how MO models a Glassport InteractionTrace WITHOUT importing
    glassport — the shape is the contract, not the class.
    """
    def mk_part(p):
        return types.SimpleNamespace(kind=p["kind"], content=p["content"])

    def mk_event(e):
        return types.SimpleNamespace(
            kind=e["kind"],
            timestamp=e.get("timestamp", ""),
            metadata=e.get("metadata", {}),
            parts=[mk_part(p) for p in e.get("parts", [])],
        )

    declared = set(shape["declared_tools"])
    events = [mk_event(e) for e in shape["events"]]
    return types.SimpleNamespace(
        declared_tools=lambda: declared,
        events=events,
    )


def project_trace_golden() -> list[dict]:
    with open(TRACE_SHAPE, encoding="utf-8") as fh:
        shape = json.load(fh)
    trace = hydrate_trace(shape)
    return [event_to_dict(ev) for ev in trace_to_events(trace)]


def main() -> None:
    events = project_golden()
    with open(GOLDEN_EVENTS, "w", encoding="utf-8") as fh:
        json.dump(events, fh, indent=2, sort_keys=True)
        fh.write("\n")
    print(f"wrote {len(events)} events -> {GOLDEN_EVENTS}")

    trace_events = project_trace_golden()
    with open(TRACE_EVENTS, "w", encoding="utf-8") as fh:
        json.dump(trace_events, fh, indent=2, sort_keys=True)
        fh.write("\n")
    print(f"wrote {len(trace_events)} events -> {TRACE_EVENTS}")


if __name__ == "__main__":
    main()
