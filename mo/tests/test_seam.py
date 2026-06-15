"""Enforce the adapter<->MO seam mechanically (premortem #4).

Importing the MO judge core must NOT pull in glassport (or any protocol lib).
If this test fails, protocol knowledge has leaked out of mo/adapters and the
A2A-portability story is broken.
"""
import subprocess
import sys


def test_mo_core_import_does_not_load_glassport():
    code = (
        "import sys;"
        "import mo.cli, mo.engine, mo.parser, mo.report, mo.trace, mo.rules, mo.ledger;"
        "leaked = [m for m in sys.modules if 'glassport' in m];"
        "assert not leaked, leaked"
    )
    proc = subprocess.run([sys.executable, "-c", code],
                          capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
