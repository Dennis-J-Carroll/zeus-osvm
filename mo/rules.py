"""Rule set parsed from a spec. In M0 these are hand-built; M1 adds the
.zspec text parser that produces the same objects.

Two rule families:
  - TriggerRule:  OBSERVE X -> EXPECT Y within W   (opens a liveness obligation)
  - AssertRule:   ASSERT <predicate>               (safety; failure -> CONDEMNED)
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable

from .events import ZeusEvent
from .ledger import Obligation


@dataclass
class TriggerRule:
    on_primitive: str            # event that fires the trigger (the OBSERVE)
    expects: str                 # primitive that satisfies the obligation
    window_ms: float             # WINDOW
    spec_line: int

    def fires_on(self, ev: ZeusEvent) -> bool:
        return ev.primitive == self.on_primitive

    def make_obligation(self, ev: ZeusEvent) -> Obligation:
        # the firing event's identifier ($tool) ties declaration to execution
        return Obligation(
            identifier=ev.identifier,
            expects=self.expects,
            opened_at=ev.ts,
            expires_at=ev.ts + self.window_ms,
            spec_line=self.spec_line,
        )


@dataclass
class AssertRule:
    spec_line: int
    # predicate(event, seen) -> True if THIS event VIOLATES the safety rule.
    # `seen` maps primitive -> set of identifiers observed so far.
    predicate: Callable[[ZeusEvent, dict], bool]
    detail: dict = field(default_factory=dict)

    def violated_by(self, ev: ZeusEvent, seen: dict) -> bool:
        return self.predicate(ev, seen)


@dataclass
class Spec:
    triggers: list[TriggerRule] = field(default_factory=list)
    assertions: list[AssertRule] = field(default_factory=list)
