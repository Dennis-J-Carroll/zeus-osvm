"""Render exact BOLT lapse-time as a self-contained HTML proof page.

    python3 -m mo.examples.lapse_time_demo --out lapse.html

M0 deferred one known wart: a BOLT's ``closed_at`` is *notice-time* — the event
timestamp that revealed the lapse — so a late flush or coarse tick made a
five-second window look like a five-second-plus obligation. The fix records the
window boundary itself as ``detail.lapsed_at`` and the reporting delay as
``detail.notice_lag_ms``, and the report tooltip shows both.

The page proves it on a real judge run: a 5s obligation opened at t=500, lapsed
at t=5500, and only noticed at t=12500. Exit code is 1 if the exact lapse is
missing or the notice lag is wrong.
"""
from __future__ import annotations

import argparse
import html
import json
import sys
from pathlib import Path

from mo.engine import run
from mo.events import ZeusEvent
from mo.parser import parse
from mo.report import jsonl_lines, render_html

SPEC_TEXT = """\
on tool_called as $tool:
    EXPECT tool_result for $tool
    WINDOW 5000
"""

EVENTS = [
    # A normal call fulfills well inside its window.
    ZeusEvent("tool_called", "quick_search", ts=0.0),
    ZeusEvent("tool_result", "quick_search", ts=250.0),
    # This one is declared at t=500, so its 5s window lapses at t=5500 ...
    ZeusEvent("tool_called", "report_export", ts=500.0),
    # ... but nothing carries a timestamp until a heartbeat at t=12500.
    ZeusEvent("heartbeat", None, ts=12500.0),
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
iframe { width: 100%; height: 320px; border: 1px solid #263241;
         border-radius: 4px; background: #0b0f0b; }
footer { margin-top: 34px; color: #6d7787; font-size: 13px; }
"""


def _esc(value: object) -> str:
    return html.escape(str(value))


def render() -> tuple[str, bool, list[dict]]:
    spec = parse(SPEC_TEXT)
    result = run(list(EVENTS), spec)
    bolts = [v for v in result.verdicts if v.kind == "BOLTED"]
    bolt = bolts[0] if len(bolts) == 1 else None
    detail = bolt.detail if bolt is not None else {}

    ok = (
        [v.kind for v in result.verdicts] == ["FULFILLED", "BOLTED"]
        and bolt is not None
        and bolt.identifier == "report_export"
        and detail.get("reason") == "liveness_window_expired"
        and detail.get("window_ms") == 5000
        and detail.get("lapsed_at") == 5500.0
        and detail.get("notice_lag_ms") == 7000.0
        and bolt.closed_at == 12500.0
        and detail.get("correlation") is None
        and "lapsed_at" not in result.verdicts[0].detail
    )

    banner = (
        '<div class="banner clean">BOLT LAPSE TIME — EXACT</div>'
        if ok else
        '<div class="banner dirty">BOLT LAPSE TIME — MISSING</div>'
    )
    metrics = f"""
    <div class="metrics">
      <div class="metric"><b>{detail.get('window_ms', '?')}ms</b><span>DECLARED WINDOW</span></div>
      <div class="metric"><b>{detail.get('lapsed_at', '?')}ms</b><span>EXACT LAPSE INSTANT</span></div>
      <div class="metric"><b>{bolt.closed_at if bolt else '?'}ms</b><span>NOTICE-TIME closed_at</span></div>
      <div class="metric"><b>{detail.get('notice_lag_ms', '?')}ms</b><span>NOTICE LAG</span></div>
    </div>"""

    event_rows = "".join(
        f"<tr><td>{_esc(ev.primitive)}</td>"
        f"<td>{_esc(ev.identifier or '—')}</td>"
        f"<td class='num'>{ev.ts:.0f}ms</td></tr>"
        for ev in EVENTS
    )
    verdicts_json = "\n".join(jsonl_lines(result))
    report = render_html(result, title="late notice · exact lapse")
    report_frame = (
        f'<iframe title="MO timeline report" srcdoc="{html.escape(report, quote=True)}">'
        "</iframe>"
    )

    page = f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>MO exact BOLT lapse time</title>
<style>{CSS}</style></head>
<body><main>
<h1>MO exact BOLT lapse time</h1>
<p class="sub">M0 deferred decision — <code>closed_at</code> is notice-time;
<code>lapsed_at</code> is the window boundary</p>
{banner}
{metrics}
<h2>EVENT STREAM</h2>
<table>
<tr><th>PRIMITIVE</th><th>IDENTIFIER</th><th class="num">TS</th></tr>
{event_rows}
</table>
<h2>VERDICT — JSONL</h2>
<pre>{_esc(verdicts_json)}</pre>
<h2>RENDERED REPORT</h2>
{report_frame}
<footer>
Hover a bar for its tooltip: a BOLT shows the declared window, the notice-time
duration, the exact lapse instant and the notice lag. The judge still learns
time only from <code>ev.ts</code>. Regenerate with
<code>python3 -m mo.examples.lapse_time_demo --out lapse.html</code>; exit code
1 means the exact lapse was not recorded.
</footer>
</main></body></html>
"""
    return page, ok, [json.loads(line) for line in jsonl_lines(result)]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", help="write the HTML proof page here")
    args = parser.parse_args(argv)

    page, ok, verdicts = render()
    if args.out:
        Path(args.out).write_text(page, encoding="utf-8")
        print(f"wrote {args.out}", file=sys.stderr)
    else:
        print(page)
    bolt = next((v for v in verdicts if v["kind"] == "BOLTED"), {})
    detail = bolt.get("detail", {})
    print(
        f"verdicts={len(verdicts)} lapsed_at={detail.get('lapsed_at')} "
        f"notice_lag_ms={detail.get('notice_lag_ms')} "
        f"result={'EXACT' if ok else 'MISSING'}",
        file=sys.stderr,
    )
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
