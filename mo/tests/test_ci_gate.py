"""Guard the CI wiring: the MO judge suite must actually be gated.

The suite is only useful as a gate if CI runs it. These tests read the real
workflow file and fail if the `mo` job disappears, loses the pytest command, or
if the C `make check` job is weakened. No YAML dependency: jobs are top-level
keys at two-space indentation, which is all this needs.
"""
from pathlib import Path

WORKFLOW = Path(__file__).resolve().parents[2] / ".github" / "workflows" / "ci.yml"


def _workflow_text() -> str:
    return WORKFLOW.read_text(encoding="utf-8")


def _job_block(text: str, name: str) -> str:
    """The lines of one top-level job, ending at the next two-space job key."""
    lines = text.splitlines()
    header = f"  {name}:"
    assert header in lines, f"no `{name}` job in {WORKFLOW}"
    start = lines.index(header)
    block = [lines[start]]
    for line in lines[start + 1:]:
        if (line.startswith("  ") and not line.startswith("   ")
                and line.rstrip().endswith(":")):
            break
        block.append(line)
    return "\n".join(block)


def test_ci_runs_the_mo_judge_suite():
    block = _job_block(_workflow_text(), "mo")

    assert "runs-on: ubuntu-latest" in block
    assert "python3 -m pip install pytest" in block
    assert "python3 -m pytest mo/ -q" in block


def test_ci_keeps_the_c_check_gate():
    block = _job_block(_workflow_text(), "check")

    assert "run: make check" in block
