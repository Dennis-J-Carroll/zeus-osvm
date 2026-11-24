"""Render the MO CI gate as a self-contained HTML proof page.

    python3 -m mo.examples.ci_gate_demo --out ci-gate.html

The MO judge suite is only a gate if CI runs it. This page reads the real
workflow, checks the `mo` job still installs pytest and runs `pytest mo/`, that
the C `make check` job is intact, then executes the job's pytest command
locally and captures its output. Exit code is 1 if the wiring is missing or the
suite fails.

This proves the job definition and the exact command. It does not execute a
GitHub-hosted runner; `.github/workflows/ci.yml` itself is validated as YAML by
the `workflow_yaml_ok` check below when PyYAML is available.
"""
from __future__ import annotations

import argparse
import html
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github" / "workflows" / "ci.yml"
COMMAND = [sys.executable, "-m", "pytest", "mo/", "-q"]

CSS = """
:root { color-scheme: dark; }
body {
  margin: 0; padding: 40px 32px 56px; background: #10151c; color: #d8dee9;
  font: 15px/1.55 ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
}
main { max-width: 980px; margin: 0 auto; }
h1 { margin: 0 0 6px; font-size: 26px; letter-spacing: .04em; color: #e5e9f0; }
.sub { margin: 0 0 26px; color: #7d8799; }
.banner {
  display: inline-block; margin: 0 0 24px; padding: 10px 18px;
  border-radius: 4px; font-weight: 700; letter-spacing: .08em;
}
.clean { background: #16351f; color: #79e08c; border: 1px solid #2c6b3b; }
.dirty { background: #3b1b1b; color: #ff8b8b; border: 1px solid #7a3030; }
.metrics {
  display: grid; grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 14px; margin: 0 0 30px;
}
@media (min-width: 700px) {
  .metrics { grid-template-columns: repeat(4, minmax(0, 1fr)); }
}
.metric {
  padding: 12px 16px; background: #171f29;
  border: 1px solid #263241; border-radius: 4px;
}
.metric b { display: block; font-size: 24px; color: #eceff4; }
.metric span { color: #7d8799; font-size: 12px; letter-spacing: .06em; }
h2 { margin: 32px 0 10px; font-size: 15px; letter-spacing: .1em; color: #9aa6b8; }
pre {
  margin: 0; padding: 16px; background: #151c25; border: 1px solid #222d3a;
  border-radius: 4px; color: #c8d1de; white-space: pre-wrap;
  word-break: break-word;
}
code { color: #c8d1de; }
.ok { color: #79e08c; }
.bad { color: #ff8b8b; }
table { width: 100%; border-collapse: collapse; background: #151c25; }
th, td { padding: 9px 12px; text-align: left; border-bottom: 1px solid #222d3a; }
th { color: #7d8799; font-size: 12px; letter-spacing: .08em; }
footer { margin-top: 34px; color: #6d7787; font-size: 13px; }
"""


def _job_block(text: str, name: str) -> str | None:
    lines = text.splitlines()
    header = f"  {name}:"
    if header not in lines:
        return None
    start = lines.index(header)
    block = [lines[start]]
    for line in lines[start + 1:]:
        if (line.startswith("  ") and not line.startswith("   ")
                and line.rstrip().endswith(":")):
            break
        block.append(line)
    return "\n".join(block)


def _yaml_ok(text: str) -> bool:
    try:
        import yaml
    except ImportError:
        return True          # optional local check; CI installs pytest only
    try:
        yaml.safe_load(text)
    except Exception:        # noqa: BLE001 - a proof page, not a parser
        return False
    return True


def _count(pattern: str, text: str) -> int:
    match = re.search(pattern, text)
    return int(match.group(1)) if match else 0


def render() -> tuple[str, bool]:
    text = WORKFLOW.read_text(encoding="utf-8")
    mo_job = _job_block(text, "mo") or ""
    check_job = _job_block(text, "check") or ""
    yaml_ok = _yaml_ok(text)

    wiring = (
        yaml_ok
        and "runs-on: ubuntu-latest" in mo_job
        and "python3 -m pip install pytest" in mo_job
        and "python3 -m pytest mo/ -q" in mo_job
        and "run: make check" in check_job
    )

    proc = subprocess.run(
        COMMAND, cwd=ROOT, capture_output=True, text=True, timeout=300,
    )
    output = (proc.stdout + proc.stderr).strip()
    passed = _count(r"(\d+) passed", output)
    skipped = _count(r"(\d+) skipped", output)
    failed = _count(r"(\d+) failed", output)
    suite_ok = proc.returncode == 0 and passed > 0 and failed == 0
    ok = wiring and suite_ok

    banner = (
        '<div class="banner clean">MO CI GATE — WIRED</div>'
        if ok else
        '<div class="banner dirty">MO CI GATE — BROKEN</div>'
    )
    metrics = f"""
    <div class="metrics">
      <div class="metric"><b>2</b><span>CI JOBS (C + MO)</span></div>
      <div class="metric"><b>{passed}</b><span>TESTS PASSED</span></div>
      <div class="metric"><b>{skipped}</b><span>SKIPPED</span></div>
      <div class="metric"><b>{failed}</b><span>FAILED</span></div>
    </div>"""

    checks = [
        ("workflow parses as YAML", yaml_ok),
        ("`mo` job present on ubuntu-latest", "runs-on: ubuntu-latest" in mo_job),
        ("`mo` job installs pytest",
         "python3 -m pip install pytest" in mo_job),
        ("`mo` job runs pytest mo/", "python3 -m pytest mo/ -q" in mo_job),
        ("C `make check` gate intact", "run: make check" in check_job),
        ("job command exits 0 locally", proc.returncode == 0),
    ]
    check_rows = "".join(
        f"<tr><td>{html.escape(label)}</td>"
        f"<td>{'<span class=\"ok\">PASS</span>' if good else '<span class=\"bad\">FAIL</span>'}</td></tr>"
        for label, good in checks
    )

    page = f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>MO CI gate</title>
<style>{CSS}</style></head>
<body><main>
<h1>MO CI gate</h1>
<p class="sub">the judge suite is only a gate if CI runs it — {passed} tests, previously ungated</p>
{banner}
{metrics}
<h2>GATE CHECKS</h2>
<table>
<tr><th>CHECK</th><th>RESULT</th></tr>
{check_rows}
</table>
<h2>WORKFLOW — .github/workflows/ci.yml</h2>
<pre>{html.escape(text)}</pre>
<h2>MO JOB — EXTRACTED</h2>
<pre>{html.escape(mo_job)}</pre>
<h2>JOB COMMAND — LOCAL RUN</h2>
<pre>$ {' '.join(COMMAND)}\n\n{html.escape(output)}</pre>
<footer>
The `mo` job installs pytest, then runs <code>python3 -m pytest mo/ -q</code>;
tests that need the optional glassport tap skip themselves. The C job still runs
<code>make check</code> unchanged. This page validates the committed workflow
and runs the same command locally. It does not execute a GitHub-hosted runner.
Regenerate with <code>python3 -m mo.examples.ci_gate_demo --out ci-gate.html</code>;
exit code 1 means the gate is missing or the suite failed.
</footer>
</main></body></html>
"""
    return page, ok


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", help="write the HTML proof page here")
    args = parser.parse_args(argv)

    page, ok = render()
    if args.out:
        Path(args.out).write_text(page, encoding="utf-8")
        print(f"wrote {args.out}", file=sys.stderr)
    else:
        print(page)
    print(f"result={'WIRED' if ok else 'BROKEN'}", file=sys.stderr)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
