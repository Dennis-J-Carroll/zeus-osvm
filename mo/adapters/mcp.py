"""MCP adapter (M2): project a Glassport InteractionTrace into a ZeusEvent
stream so MO can judge a real captured MCP session.

This is the ONLY place in MO that knows about MCP/Glassport. The judge core
(engine, ledger, rules, parser) never imports this module — that seam is what
keeps MO protocol-agnostic and the A2A story alive (premortem #4). A test
(test_seam.py) enforces it mechanically.

Two layers:
  * trace_to_events(trace)        pure projection, NO glassport import. Works on
                                  any object exposing .declared_tools() and an
                                  .events list of InteractionTrace-shaped events.
  * from_mcp_session[_file](...)  convenience that lazily calls Glassport's
                                  from_mcp_session to build the trace first.

Projection rules:
  * declared tools  -> one `tool_declared` per tool, emitted FIRST at the
    earliest trace timestamp. Declarations are a static fact of the session;
    emitting them up front (not at the late tools/list *response*) is what lets
    the fabrication ASSERT see the declared set before any call is judged.
  * TOOL_CALL       -> `tool_called`,  identifier = tool name, payload = args
  * TOOL_RESULT     -> `tool_result`,  identifier = tool name, payload.is_error
  * everything else -> a generic event named after its kind, identifier None,
    so nothing is silently dropped and coverage (premortem #5) stays honest.
"""
from __future__ import annotations

from typing import Iterable, Iterator

from ..events import ZeusEvent

# InteractionTrace enums subclass str, so comparing against these literals
# duck-types cleanly without importing glassport.
_KIND_TOOL_CALL = "tool_call"
_KIND_TOOL_RESULT = "tool_result"
_PART_TOOL_USE = "tool_use"
_PART_TOOL_RESULT = "tool_result"


def _to_ms(event) -> float:
    ts = getattr(event, "timestamp", "") or ""
    if ts:
        try:
            from datetime import datetime
            return datetime.fromisoformat(ts).timestamp() * 1000.0
        except ValueError:
            pass
    # fall back to the wire sequence number so ordering survives missing ts
    seq = (getattr(event, "metadata", None) or {}).get("seq")
    return float(seq) if seq is not None else 0.0


def _tool_call_name(event) -> str | None:
    for part in getattr(event, "parts", []):
        if part.kind == _PART_TOOL_USE and isinstance(part.content, dict):
            return part.content.get("name")
    return None


def _tool_call_args(event) -> dict:
    for part in getattr(event, "parts", []):
        if part.kind == _PART_TOOL_USE and isinstance(part.content, dict):
            return part.content.get("arguments", {})
    return {}


def _result_is_error(event) -> bool:
    for part in getattr(event, "parts", []):
        if part.kind == _PART_TOOL_RESULT and isinstance(part.content, dict):
            return bool(part.content.get("is_error", False))
    return False


def trace_to_events(trace) -> Iterator[ZeusEvent]:
    events = list(getattr(trace, "events", []))
    first_ts = min((_to_ms(e) for e in events), default=0.0)

    # declarations up front, sorted for deterministic replay
    for name in sorted(trace.declared_tools()):
        yield ZeusEvent("tool_declared", name, payload={}, ts=first_ts)

    for e in events:
        ts = _to_ms(e)
        if e.kind == _KIND_TOOL_CALL:
            yield ZeusEvent("tool_called", _tool_call_name(e),
                            payload={"arguments": _tool_call_args(e)}, ts=ts)
        elif e.kind == _KIND_TOOL_RESULT:
            name = (getattr(e, "metadata", None) or {}).get("tool_name")
            yield ZeusEvent("tool_result", name,
                            payload={"is_error": _result_is_error(e)}, ts=ts)
        else:
            yield ZeusEvent(str(e.kind), None, payload={}, ts=ts)


def from_mcp_session(log_lines: Iterable[str], **kw) -> Iterator[ZeusEvent]:
    from glassport.adapters.mcp_session import from_mcp_session as _gp
    return trace_to_events(_gp(log_lines, **kw))


def from_mcp_session_file(path, **kw) -> Iterator[ZeusEvent]:
    with open(path, encoding="utf-8") as fh:
        return from_mcp_session(list(fh), **kw)
