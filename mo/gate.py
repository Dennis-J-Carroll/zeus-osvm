"""Pre-delivery gate: judge an effect before it happens.

The post-hoc MO verdict is a diagnostic. A *gate* sits at the effect seam and
answers one question before delivery: is this effect accounted for by the spec?

    ALLOW     a rule claimed the event (it triggers, fulfils, or is explicitly
              condemned — the last one is DENY, see below)
    DENY      an ASSERT matched; the effect must not happen
    ESCALATE  nothing claimed it. Silence is not permission (premortem #5):
              an unmatched effect goes to a human, never through as "fine".

Two properties matter more than the decision vocabulary:

* Non-finalizing. `check()` never emits end-of-stream BOLTs. The caller decides
  when the session ends.
* Non-committing on refusal. The event is judged on a forked ledger; only an
  ALLOW commits it. A denied or escalated effect therefore leaves no trace in
  MO's state, so judging it did not pretend it happened.
"""
from __future__ import annotations

from dataclasses import dataclass, field

from .engine import Judge, Verdict
from .rules import Spec

ALLOW = "allow"
DENY = "deny"
ESCALATE = "escalate"


@dataclass
class Decision:
    action: str
    reason: str
    verdicts: list[Verdict] = field(default_factory=list)


class Gate:
    def __init__(self, spec: Spec) -> None:
        self._judge = Judge(spec)

    def check(self, ev) -> Decision:
        """Decide one event without finalising and without committing a refusal."""
        trial = self._judge.fork()
        observation = trial.observe(ev)

        if any(v.kind == "CONDEMNED" for v in observation.verdicts):
            return Decision(DENY, "assertion_violated", observation.verdicts)

        if not observation.matched:
            return Decision(ESCALATE, "unmatched_effect", observation.verdicts)

        self._judge = trial
        return Decision(ALLOW, "matched_rule", observation.verdicts)

    def finish(self) -> list[Verdict]:
        """End the session and bolt whatever is still open."""
        return self._judge.finish()
