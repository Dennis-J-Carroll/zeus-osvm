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
    spec_source: str | None = None   # fragment file the rule came from

    def fires_on(self, ev: ZeusEvent) -> bool:
        return ev.primitive == self.on_primitive

    def make_obligation(self, ev: ZeusEvent) -> Obligation:
        # the firing event's identifier ($tool) ties declaration to execution;
        # its correlation id (if any) keeps concurrent calls to the same tool
        # from cross-closing (premortem #2).
        return Obligation(
            identifier=ev.identifier,
            expects=self.expects,
            opened_at=ev.ts,
            expires_at=ev.ts + self.window_ms,
            spec_line=self.spec_line,
            correlation=ev.correlation,
            spec_source=self.spec_source,
        )


@dataclass
class AssertRule:
    spec_line: int
    # predicate(event, seen) -> True if THIS event VIOLATES the safety rule.
    # `seen` maps primitive -> set of identifiers observed so far.
    predicate: Callable[[ZeusEvent, dict], bool]
    detail: dict = field(default_factory=dict)
    # Optional wire-order grace window. When set, a violation opens a pending
    # check instead of condemning immediately; a later event of `primitive_b`
    # with the same identifier resolves it if it arrives by the deadline.
    # Immediate ASSERTs leave grace_ms/primitive_a/primitive_b unset.
    grace_ms: float | None = None
    primitive_a: str | None = None
    primitive_b: str | None = None
    spec_source: str | None = None   # fragment file the rule came from

    def violated_by(self, ev: ZeusEvent, seen: dict) -> bool:
        return self.predicate(ev, seen)

    @property
    def deferred(self) -> bool:
        return self.grace_ms is not None


@dataclass
class Spec:
    triggers: list[TriggerRule] = field(default_factory=list)
    assertions: list[AssertRule] = field(default_factory=list)
