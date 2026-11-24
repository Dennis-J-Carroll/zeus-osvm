"""Render the declaration/execution correlation fix as an HTML proof page.

    python3 -m mo.examples.declaration_correlation_demo --out proof.html

Correlation ids were added for premortem #2 (two concurrent calls to one tool
must not cross-close). The matcher then required agreement whenever *either*
side carried an id. A declaration carries none — so every correlated call was
rejected by its own declaration's obligation, and the canonical
`every_tool_honored.zspec` BOLTED tools it had just watched being called.

The fix scopes correlation to the obligation: only an obligation opened by an
instance event constrains matching. A declaration's obligation accepts the
first matching execution. The guard below proves the original concurrency
protection is intact (a result closes only its own call).

Exit code is 1 if the declaration run does not FULFILL or the guard
cross-closes.
"""
from __future__ import annotations

import argparse
import html
import json
import sys
from dataclasses import replace
from pathlib import Path

from mo.adapters.mcp import frames_to_events
from mo.engine import run
from mo.events import ZeusEvent
from mo.parser import parse
from mo.report import jsonl_lines, render_html

SPEC_TEXT = """\
on tool_declared as $tool:
    EXPECT tool_called for $tool
    WINDOW 5000

ASSERT no tool_called where identifier not in tool_declared
"""

GUARD_SPEC = """\
on tool_called as $tool:
    EXPECT tool_result for $tool
    WINDOW 5000
"""

MCP_FRAMES = [
    # tools/list response: declares `search`, no jsonrpc id on the event
    {"dir": "s2c", "ts": "2026-11-24T10:00:00.000000+00:00",
     "frame": {"jsonrpc": "2.0", "id": 1,
               "result": {"tools": [{"name": "search"}]}}},
    # the agent honors it, with jsonrpc id 7 as its instance id
    {"dir": "c2s", "ts": "2026-11-24T10:00:00.200000+00:00",
     "frame": {"jsonrpc": "2.0", "id": 7, "method": "tools/call",
               "params": {"name": "search", "arguments": {"q": "zeus"}}}},
    # result returns on the same id
    {"dir": "s2c", "ts": "2026-11-24T10:00:00.400000+00:00",
     "frame": {"jsonrpc": "2.0", "id": 7, "result": {"content": []}}},
]

GUARD_EVENTS = [
    ZeusEvent("tool_called", "search", correlation="a", ts=0.0),
    ZeusEvent("tool_called", "search", correlation="b", ts=10.0),
    ZeusEvent("tool_result", "search", correlation="a", ts=20.0),
]

BEFORE = """if (ob.correlation is not None or ev.correlation is not None) \\
        and ev.correlation != ob.correlation:
    continue          # <- declaration (None) vs call ("7"): always skipped"""

AFTER = """if ob.correlation is not None and ev.correlation != ob.correlation:
    continue          # <- only the obligation's own instance id constrains"""

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
pre {
  margin: 0; padding: 16px; background: #151c25; border: 1px solid #222d3a;
  border-radius: 4px; color: #c8d1de; white-space: pre-wrap;
  word-break: break-word;
}
code { color: #c8d1de; }
.ok { color: #79e08c; }
.bad { color: #ff8b8b; }
iframe { width: 100%; height: 320px; border: 1px solid #263241;
         border-radius: 4px; background: #0b0f0b; }
footer { margin-top: 34px; color: #6d7787; font-size: 13px; }
"""


def _esc(value: object) -> str:
    return html.escape(str(value))


def _declaration_events() -> list[ZeusEvent]:
    events = list(frames_to_events(MCP_FRAMES))
    base = events[0].ts
    return [replace(e, ts=e.ts - base) for e in events]


def render() -> tuple[str, bool]:
    events = _declaration_events()
    decl = run(events, parse(SPEC_TEXT))
    guard = run(GUARD_EVENTS, parse(GUARD_SPEC))

    decl_fulfilled = [v for v in decl.verdicts if v.kind == "FULFILLED"]
    decl_bolted = [v for v in decl.verdicts if v.kind == "BOLTED"]
    decl_condemned = [v for v in decl.verdicts if v.kind == "CONDEMNED"]

    guard_fulfilled = [v for v in guard.verdicts if v.kind == "FULFILLED"]
    guard_bolted = [v for v in guard.verdicts if v.kind == "BOLTED"]

    ok = (
        [v.kind for v in decl.verdicts] == ["FULFILLED"]
        and decl_fulfilled[0].identifier == "search"
        and len(decl_bolted) == 0
        and len(decl_condemned) == 0
        and [v.detail.get("correlation") for v in guard_fulfilled] == ["a"]
        and [v.detail.get("correlation") for v in guard_bolted] == ["b"]
    )

    banner = (
        '<div class="banner clean">DECLARATION → CORRELATED CALL — FULFILLED</div>'
        if ok else
        '<div class="banner dirty">DECLARATION → CORRELATED CALL — BOLTED</div>'
    )
    metrics = f"""
    <div class="metrics">
      <div class="metric"><b>1</b><span>DECLARED TOOL</span></div>
      <div class="metric"><b>1</b><span>CORRELATED CALL (id 7)</span></div>
      <div class="metric"><b>{len(decl_fulfilled)}</b><span>FULFILLED</span></div>
      <div class="metric"><b>{len(decl_bolted)}</b><span>BOLTED</span></div>
    </div>"""

    event_rows = "".join(
        f"<tr><td>{_esc(ev.primitive)}</td>"
        f"<td>{_esc(ev.identifier or '—')}</td>"
        f"<td>{_esc(ev.correlation or '—')}</td>"
        f"<td class='num'>{ev.ts:.0f}ms</td></tr>"
        for ev in events
    )
    verdicts_json = "\n".join(jsonl_lines(decl))
    guard_json = "\n".join(jsonl_lines(guard))
    report = render_html(decl, title="declaration honored")
    report_frame = (
        f'<iframe title="MO timeline report" srcdoc="{html.escape(report, quote=True)}">'
        "</iframe>"
    )

    page = f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>MO declaration correlation</title>
<style>{CSS}</style></head>
<body><main>
<h1>MO declaration → execution correlation</h1>
<p class="sub">a correlated call must be allowed to honor an uncorrelated
declaration — previously it could not</p>
{banner}
{metrics}
<h2>WHY IT BOLTED</h2>
<pre>BEFORE\n{BEFORE}\n\nAFTER\n{AFTER}</pre>
<h2>PROJECTED MCP SESSION</h2>
<table>
<tr><th>PRIMITIVE</th><th>IDENTIFIER</th><th>CORRELATION</th>
<th class="num">TS</th></tr>
{event_rows}
</table>
<h2>VERDICT — JSONL</h2>
<pre>{_esc(verdicts_json)}</pre>
<h2>RENDERED REPORT</h2>
{report_frame}
<h2>GUARD — CONCURRENT SAME-TOOL CALLS</h2>
<p class="sub">premortem #2 must still hold: a result closes only its own call</p>
<pre>{_esc(guard_json)}</pre>
<footer>
The declaration event has no instance id; the call does. Matching is now
scoped to the obligation's own correlation, so the declaration's obligation
accepts the first execution, while a call opened with an id still demands the
same id on its result. Demo of the false-BOLT fix for
<code>every_tool_honored.zspec</code>. Regenerate with
<code>python3 -m mo.examples.declaration_correlation_demo --out proof.html</code>;
exit code 1 means the declaration run BOLTED or the guard cross-closed.
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
    print(f"result={'FIXED' if ok else 'BOLTED'}", file=sys.stderr)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
