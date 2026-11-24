"""Render the replay-determinism guard as a self-contained HTML proof page.

    python3 -m mo.examples.determinism_guard_demo --out determinism.html

The page reports the real lint result over the judge core plus synthetic probes
that prove the lint catches direct, aliased and attribute clock access. Exit
code is 1 if the core is dirty or a probe is missed.
"""
from __future__ import annotations

import argparse
import html
import sys
from pathlib import Path

from mo.determinism import CORE_MODULES, scan_core, scan_source

PROBES = [
    ("direct import", "import time\nnow = time.time()\n", True),
    ("aliased import", "from time import monotonic as tick\ntick()\n", True),
    ("datetime.now", "import datetime\nstamp = datetime.datetime.now()\n", True),
    ("module alias", "import time as clock\nclock.sleep(1)\n", True),
    ("event ts only", "def deadline(ev):\n    return ev.ts + 5000\n", False),
]


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
table { width: 100%; border-collapse: collapse; background: #151c25; }
th, td { padding: 9px 12px; text-align: left; border-bottom: 1px solid #222d3a; }
th { color: #7d8799; font-size: 12px; letter-spacing: .08em; }
td.num { text-align: right; }
code, pre { color: #c8d1de; }
pre { margin: 0; white-space: pre-wrap; word-break: break-word; }
.ok { color: #79e08c; }
.bad { color: #ff8b8b; }
footer { margin-top: 34px; color: #6d7787; font-size: 13px; }
"""


def _esc(value: object) -> str:
    return html.escape(str(value))


def render() -> tuple[str, bool, int, int]:
    core_findings = scan_core()
    total_lines = 0
    module_rows: list[str] = []
    per_module: dict[str, int] = {name: 0 for name in CORE_MODULES}
    for finding in core_findings:
        per_module[finding.path.split("/", 1)[1]] += 1

    for name in CORE_MODULES:
        path = Path(__file__).resolve().parent.parent / name
        lines = len(path.read_text(encoding="utf-8").splitlines())
        total_lines += lines
        hits = per_module[name]
        status = '<span class="ok">CLEAN</span>' if hits == 0 else '<span class="bad">CLOCK READ</span>'
        module_rows.append(
            f"<tr><td>mo/{_esc(name)}</td><td class='num'>{lines}</td>"
            f"<td class='num'>{hits}</td><td>{status}</td></tr>"
        )

    probe_rows: list[str] = []
    probes_ok = True
    probes_passed = 0
    for label, source, should_flag in PROBES:
        findings = scan_source(source, f"<{label}>")
        detected = bool(findings)
        passed = detected == should_flag
        probes_ok = probes_ok and passed
        probes_passed += 1 if passed else 0
        outcome = '<span class="ok">PASS</span>' if passed else '<span class="bad">FAIL</span>'
        probe_rows.append(
            f"<tr><td>{_esc(label)}</td>"
            f"<td><pre>{_esc(source.rstrip())}</pre></td>"
            f"<td>{'clock read' if should_flag else 'clean'}</td>"
            f"<td>{'clock read' if detected else 'clean'}</td>"
            f"<td>{outcome}</td></tr>"
        )

    clean = not core_findings and probes_ok
    banner = (
        '<div class="banner clean">REPLAY DETERMINISM — ENFORCED</div>'
        if clean else
        '<div class="banner dirty">REPLAY DETERMINISM — VIOLATED</div>'
    )
    metrics = f"""
    <div class="metrics">
      <div class="metric"><b>{len(CORE_MODULES)}</b><span>CORE MODULES SCANNED</span></div>
      <div class="metric"><b>{total_lines}</b><span>SOURCE LINES</span></div>
      <div class="metric"><b>{len(core_findings)}</b><span>CLOCK READS IN CORE</span></div>
      <div class="metric"><b>{probes_passed}/{len(PROBES)}</b><span>PROBES PASSED</span></div>
    </div>"""

    page = f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>MO replay determinism guard</title>
<style>{CSS}</style></head>
<body><main>
<h1>MO replay determinism guard</h1>
<p class="sub">premortem #6 — the judge may only learn time from <code>ev.ts</code></p>
{banner}
{metrics}
<h2>JUDGE CORE — STATIC SCAN</h2>
<table>
<tr><th>MODULE</th><th class="num">LINES</th><th class="num">CLOCK READS</th><th>STATUS</th></tr>
{''.join(module_rows)}
</table>
<h2>LINT SELF-TEST — SYNTHETIC PROBES</h2>
<table>
<tr><th>CASE</th><th>SOURCE</th><th>EXPECTED</th><th>DETECTED</th><th>RESULT</th></tr>
{''.join(probe_rows)}
</table>
<footer>
The lint fails on any <code>time</code>, <code>datetime</code> or
<code>timeit</code> import or call reachable from the verdict path, including
aliased imports. Adapters and the CLI are excluded: that is where live ticks
are minted. Run <code>python3 -m pytest mo/</code> to enforce it, or
<code>python3 -m mo.examples.determinism_guard_demo --out determinism.html</code>
to regenerate this page. Exit code 1 means a clock read reached the judge.
</footer>
</main></body></html>
"""
    return page, clean, len(core_findings), total_lines


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", help="write the HTML proof page here")
    args = parser.parse_args(argv)

    page, clean, findings, lines = render()
    if args.out:
        Path(args.out).write_text(page, encoding="utf-8")
        print(f"wrote {args.out}", file=sys.stderr)
    else:
        print(page)
    print(
        f"modules={len(CORE_MODULES)} lines={lines} clock_reads={findings} "
        f"probes={len(PROBES)} result={'CLEAN' if clean else 'VIOLATED'}",
        file=sys.stderr,
    )
    return 0 if clean else 1


if __name__ == "__main__":
    raise SystemExit(main())
