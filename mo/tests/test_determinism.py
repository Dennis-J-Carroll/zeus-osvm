"""Enforce replay determinism mechanically (premortem #6).

The engine may only advance time from event timestamps. If any verdict path
reads the wall clock, the same trace stops reproducing, so the lint in
`mo/determinism.py` is run over the core on every test run.
"""
from pathlib import Path

import mo
from mo.determinism import CORE_MODULES, scan_core, scan_source

CORE = Path(mo.__file__).resolve().parent


def test_judge_core_never_reads_the_wall_clock():
    findings = scan_core()
    assert findings == [], "\n".join(
        f"{f.path}:{f.line}: {f.symbol} ({f.reason})" for f in findings
    )


def test_lint_catches_direct_and_aliased_clock_access():
    probes = [
        "import time\nnow = time.time()\n",
        "from time import monotonic as tick\ntick()\n",
        "import datetime\nstamp = datetime.datetime.now()\n",
        "from datetime import datetime as dt\nstamp = dt.utcnow()\n",
        "import time as clock\nclock.sleep(1)\n",
    ]
    for probe in probes:
        assert scan_source(probe, "<probe>"), probe


def test_lint_allows_event_timestamps():
    clean = (
        "def deadline(ev):\n"
        "    return ev.ts + 5000\n"
        "\n"
        "def label(ev):\n"
        "    return {'at': ev.ts}\n"
    )
    assert scan_source(clean, "<probe>") == []
    assert set(CORE_MODULES) <= {p.name for p in CORE.glob("*.py")}
