"""CI guard for the live dogfood demo (mo/examples/live_dogfood.py).

This is the automated form of the `kimi_look.md` step-4 manual run: a server
that answers one call and hangs on the next must yield exactly one FULFILLED and
one BOLT, with the BOLT minted mid-session by a synthetic tick (premortem #1).

It exercises the REAL live path (wall-clock tail), so it's timing-dependent and
a touch slower than the rest of the suite. Marked so it can be deselected:
`pytest -m "not slow"`.
"""
import pytest

from mo.examples.live_dogfood import main


@pytest.mark.slow
def test_live_dogfood_fires_one_bolt_midsession():
    assert main() == 0
