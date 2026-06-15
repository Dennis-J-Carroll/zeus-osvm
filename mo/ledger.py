"""The obligation ledger: MO's core data structure.

Not a linear cursor. Each obligation carries its own clock and closes
independently of intervening traffic (satisfaction-in-window, not ordering).
"""
from __future__ import annotations

from dataclasses import dataclass


@dataclass
class Obligation:
    identifier: str | None
    expects: str                 # primitive that satisfies it
    opened_at: float
    expires_at: float            # opened_at + window
    spec_line: int
    correlation: str | None = None   # instance id carried from trigger event
    state: str = "open"          # "open" | "fulfilled" | "bolted"


class ObligationLedger:
    def __init__(self) -> None:
        self._obligations: list[Obligation] = []

    def open(self, ob: Obligation) -> None:
        self._obligations.append(ob)

    def open_obligations(self):
        return [ob for ob in self._obligations if ob.state == "open"]

    def close(self, ob: Obligation, state: str) -> None:
        ob.state = state
