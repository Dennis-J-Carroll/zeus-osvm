"""Wire-level events the adapter emits into MO.

MO speaks only Zeus primitives here. No MCP/A2A shapes leak across this
boundary (premortem #4): everything the judge sees is a ZeusEvent.
"""
from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class ZeusEvent:
    primitive: str               # "tool_declared", "tool_called", "send", ...
    identifier: str | None       # binding key, e.g. tool name
    correlation: str | None = field(default=None, kw_only=True)  # instance id
    payload: dict = field(default_factory=dict)
    ts: float = 0.0              # monotonic ms; time only ever advances via events
