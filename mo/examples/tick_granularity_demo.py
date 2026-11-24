"""Measure live tick granularity and prove the §8 decision.

    python3 -m mo.examples.tick_granularity_demo --out tick.html

The plan said "start 100ms, measure". The measurement: a BOLT is noticed by
the first tick after its window boundary, so the effective tick period *is*
the notice-lag budget. A fixed 100ms tick is 2% of a 5s window (fine) but a
whole window late for a 50ms obligation. The decision encoded here:

  * keep 100ms as the ceiling for ordinary windows,
  * tighten to one tenth of the tightest declared window (5ms floor),
  * shrink the poll to half the tick (50ms cap) so the tick is observable,
  * emit one final tick at an idle cutoff so a lapse in the last quiet
    interval is reported as a lapse, not as an end-of-session BOLT.

The page drives the real tailer on a virtual clock to measure tick gaps, then
drives the real judge for a 50ms window (auto tick) and for a coarse tick cut
off by idle. Exit code is 1 if any measured claim is false.
"""
from __future__ import annotations

import argparse
import html
import io
import json
import math
import sys
import tempfile
from pathlib import Path

from mo.adapters.live import (
    DEFAULT_TICK_MS,
    MIN_TICK_MS,
    auto_poll_s,
    auto_tick_ms,
    tail_events,
)
from mo.cli import load_spec, watch
from mo.report import jsonl_lines, render_html
from mo.rules import Spec, TriggerRule

WINDOWS_MS = [50, 100, 500, 1000, 5000]
COARSE_TICK_MS = 1000.0   # deliberately not due before the idle cutoff
COARSE_IDLE_MS = 60.0
SHORT_WINDOW_MS = 50.0

SHORT_SPEC = """\
on tool_called as $tool:
    EXPECT tool_result for $tool
    WINDOW 50
"""

# No `ts`, so the live projector stamps this frame from its seq (1.0ms).
SESSION_RECORD = {
    "dir": "c2s", "seq": 1,
    "frame": {"id": 5, "method": "tools/call",
              "params": {"name": "search", "arguments": {}}},
}

POLICY_CODE = """tick_ms = max(5, min(100, min(window_ms) / 10))
poll_ms = min(50, tick_ms / 2)"""

IDLE_CODE = """if idle_ms is not None and now - last_line >= idle_ms:
    if now > last_tick:
        yield ZeusEvent("tick", None, ts=now)   # notice a lapse at the cutoff
    return"""

CSS = """
:root { color-scheme: dark; }
body {
  margin: 0; padding: 40px 32px 56px; background: #10151c; color: #d8dee9;
  font: 15px/1.55 ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
}
main { max-width: 1040px; margin: 0 auto; }
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
h2 {
  margin: 34px 0 10px; font-size: 15px; letter-spacing: .1em;
  color: #9aa6b8;
}
table { width: 100%; border-collapse: collapse; background: #151c25; }
th, td { padding: 9px 12px; text-align: left; border-bottom: 1px solid #222d3a; }
th { color: #7d8799; font-size: 12px; letter-spacing: .08em; }
td.num { text-align: right; }
code, pre { color: #c8d1de; }
pre {
  margin: 0; padding: 14px 16px; background: #151c25;
  border: 1px solid #263241; border-radius: 4px;
  white-space: pre-wrap; word-break: break-word;
}
.ok { color: #79e08c; }
.bad { color: #ff8b8b; }
.note { margin: 10px 0 0; color: #7d8799; font-size: 13px; }
iframe {
  width: 100%; height: 300px; border: 1px solid #263241;
  border-radius: 4px; background: #0b0f0b;
}
footer { margin-top: 34px; color: #6d7787; font-size: 13px; }
"""


def _esc(value: object) -> str:
    return html.escape(str(value))


def _fmt(value: float) -> str:
    text = f"{value:.2f}".rstrip("0").rstrip(".")
    return text or "0"


def _window_spec(ms: float) -> Spec:
    return Spec(triggers=[
        TriggerRule("tool_called", "tool_result", window_ms=ms, spec_line=1),
    ])


def _effective_period_ms(tick_ms: float, poll_s: float) -> float:
    """The tailer mints ticks on the poll grid, so this is the real interval."""
    poll_ms = poll_s * 1000.0
    return math.ceil(tick_ms / poll_ms - 1e-9) * poll_ms


class _StepClock:
    """A virtual clock that advances one poll per reading (no wall time)."""

    def __init__(self, step_ms: float) -> None:
        self.t = 0.0
        self.step = step_ms

    def __call__(self) -> float:
        now = self.t
        self.t += self.step
        return now


def _policy_rows() -> list[dict]:
    rows = []
    for window in WINDOWS_MS:
        tick = auto_tick_ms(_window_spec(window))
        poll = auto_poll_s(tick)
        period = _effective_period_ms(tick, poll)
        rows.append({
            "window_ms": window,
            "tick_ms": tick,
            "poll_ms": poll * 1000.0,
            "period_ms": period,
            "worst_lag_ms": period,
            "lag_pct": period / window * 100.0,
            "polls_per_s": 1000.0 / (poll * 1000.0),
            "ok": (tick <= DEFAULT_TICK_MS and period <= window * 0.1001),
        })
    return rows


def _measure_gaps(tick_ms: float, poll_s: float, span_ms: float = 400.0):
    """Run the real tailer on an empty file and time its ticks with a fake clock."""
    clock = _StepClock(poll_s * 1000.0)
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "empty.jsonl"
        path.write_text("", encoding="utf-8")
        events = list(tail_events(
            str(path), project=lambda rec: iter(()),
            tick_ms=tick_ms, poll_s=poll_s,
            clock=clock, sleep=lambda *_: None,
            stop=lambda: clock.t > span_ms,
        ))
    ticks = [ev.ts for ev in events if ev.primitive == "tick"]
    gaps = [b - a for a, b in zip(ticks, ticks[1:])]
    return ticks, gaps


def _gap_rows() -> list[dict]:
    rows = []
    for tick in (DEFAULT_TICK_MS, MIN_TICK_MS):
        poll = auto_poll_s(tick)
        ticks, gaps = _measure_gaps(tick, poll)
        expected = _effective_period_ms(tick, poll)
        worst_error = max((abs(g - expected) for g in gaps), default=float("inf"))
        rows.append({
            "tick_ms": tick,
            "poll_ms": poll * 1000.0,
            "count": len(ticks),
            "min_gap_ms": min(gaps) if gaps else 0.0,
            "max_gap_ms": max(gaps) if gaps else 0.0,
            "expected_ms": expected,
            "ok": bool(gaps) and worst_error < 1e-6,
        })
    return rows


def _short_spec_file(tmp: Path) -> Path:
    path = tmp / "short.zspec"
    path.write_text(SHORT_SPEC + "\n", encoding="utf-8")
    return path


def _auto_run():
    clock = _StepClock(2.5)
    with tempfile.TemporaryDirectory() as tmp:
        spec_path = _short_spec_file(Path(tmp))
        session = Path(tmp) / "session.jsonl"
        session.write_text(json.dumps(SESSION_RECORD) + "\n", encoding="utf-8")
        out = io.StringIO()
        result = watch(
            load_spec(str(spec_path)), str(session),
            clock=clock, sleep=lambda *_: None,
            stop=lambda: clock.t > 300.0, out=out,
        )
    return result, out.getvalue()


def _idle_run():
    # 50ms poll = the auto poll for the 1000ms override. The 50ms window
    # lapses at 51ms; idle returns at 150ms, before the first due tick.
    clock = _StepClock(50.0)
    with tempfile.TemporaryDirectory() as tmp:
        spec_path = _short_spec_file(Path(tmp))
        session = Path(tmp) / "session.jsonl"
        session.write_text(json.dumps(SESSION_RECORD) + "\n", encoding="utf-8")
        out = io.StringIO()
        result = watch(
            load_spec(str(spec_path)), str(session),
            tick_ms=COARSE_TICK_MS, idle_ms=COARSE_IDLE_MS,
            clock=clock, sleep=lambda *_: None, out=out,
        )
    return result, out.getvalue()


def _bolt(result):
    bolts = [v for v in result.verdicts if v.kind == "BOLTED"]
    return bolts[0] if len(bolts) == 1 else None


def _auto_ok(result) -> bool:
    bolt = _bolt(result)
    return (
        bolt is not None
        and bolt.identifier == "search"
        and bolt.detail.get("reason") == "liveness_window_expired"
        and bolt.detail.get("window_ms") == SHORT_WINDOW_MS
        and bolt.detail.get("lapsed_at") == bolt.opened_at + SHORT_WINDOW_MS
        and 0 < bolt.detail.get("notice_lag_ms", 0) <= 10.0
    )


def _idle_ok(result) -> bool:
    bolt = _bolt(result)
    if bolt is None:
        return False
    lapsed = bolt.opened_at + SHORT_WINDOW_MS
    return (
        bolt.identifier == "search"
        and bolt.detail.get("reason") == "liveness_window_expired"
        and bolt.detail.get("lapsed_at") == lapsed
        and bolt.closed_at < COARSE_TICK_MS   # the cutoff, not the due tick
        and bolt.detail.get("notice_lag_ms") == bolt.closed_at - lapsed
    )


def _table(headers: list[str], rows: list[list[str]]) -> str:
    head = "".join(f"<th>{_esc(h)}</th>" for h in headers)
    body = "".join(
        "<tr>" + "".join(f"<td>{cell}</td>" for cell in row) + "</tr>"
        for row in rows
    )
    return f"<table><tr>{head}</tr>{body}</table>"


def render() -> tuple[str, bool, dict]:
    policy = _policy_rows()
    gaps = _gap_rows()
    auto_result, auto_jsonl = _auto_run()
    idle_result, idle_jsonl = _idle_run()

    auto_bolt = _bolt(auto_result)
    idle_bolt = _bolt(idle_result)
    auto_ok = _auto_ok(auto_result)
    idle_ok = _idle_ok(idle_result)
    policy_ok = (
        all(row["ok"] for row in policy)
        and auto_tick_ms(_window_spec(5000)) == DEFAULT_TICK_MS
        and auto_tick_ms(_window_spec(50)) == MIN_TICK_MS
    )
    gaps_ok = all(row["ok"] for row in gaps)
    ok = policy_ok and gaps_ok and auto_ok and idle_ok

    worst_pct = max(row["lag_pct"] for row in policy)
    auto_lag = auto_bolt.detail["notice_lag_ms"] if auto_bolt else None
    idle_lag = idle_bolt.detail["notice_lag_ms"] if idle_bolt else None

    banner = (
        '<div class="banner clean">TICK GRANULARITY — MEASURED</div>'
        if ok else
        '<div class="banner dirty">TICK GRANULARITY — UNPROVEN</div>'
    )
    metrics = f"""
    <div class="metrics">
      <div class="metric"><b>{_fmt(DEFAULT_TICK_MS)}ms</b><span>TICK CEILING</span></div>
      <div class="metric"><b>{worst_pct:.1f}%</b><span>WORST LAG / WINDOW</span></div>
      <div class="metric"><b>{_fmt(MIN_TICK_MS)}ms</b><span>TICK FLOOR</span></div>
      <div class="metric"><b>{_fmt(auto_lag) if auto_lag is not None else '?'}ms</b><span>50ms BOLT LAG</span></div>
    </div>"""

    policy_html = _table(
        ["WINDOW", "AUTO TICK", "POLL", "EFFECTIVE PERIOD",
         "WORST LAG", "LAG / WINDOW", "POLLS/S", "OK"],
        [
            [
                f"<td class='num'>{row['window_ms']}ms</td>",
                f"<td class='num'>{_fmt(row['tick_ms'])}ms</td>",
                f"<td class='num'>{_fmt(row['poll_ms'])}ms</td>",
                f"<td class='num'>{_fmt(row['period_ms'])}ms</td>",
                f"<td class='num'>{_fmt(row['worst_lag_ms'])}ms</td>",
                f"<td class='num'>{row['lag_pct']:.1f}%</td>",
                f"<td class='num'>{row['polls_per_s']:.0f}</td>",
                f"<td class='{'ok' if row['ok'] else 'bad'}'>"
                f"{'PASS' if row['ok'] else 'FAIL'}</td>",
            ]
            for row in policy
        ],
    )

    gaps_html = _table(
        ["REQUESTED TICK", "POLL", "TICKS", "MIN GAP", "MAX GAP",
         "EXPECTED PERIOD", "OK"],
        [
            [
                f"<td class='num'>{_fmt(row['tick_ms'])}ms</td>",
                f"<td class='num'>{_fmt(row['poll_ms'])}ms</td>",
                f"<td class='num'>{row['count']}</td>",
                f"<td class='num'>{_fmt(row['min_gap_ms'])}ms</td>",
                f"<td class='num'>{_fmt(row['max_gap_ms'])}ms</td>",
                f"<td class='num'>{_fmt(row['expected_ms'])}ms</td>",
                f"<td class='{'ok' if row['ok'] else 'bad'}'>"
                f"{'PASS' if row['ok'] else 'FAIL'}</td>",
            ]
            for row in gaps
        ],
    )

    auto_metrics = ""
    if auto_bolt is not None:
        auto_metrics = f"""
        <div class="metrics">
          <div class="metric"><b>{_fmt(auto_bolt.opened_at)}ms</b><span>OPENED</span></div>
          <div class="metric"><b>{_fmt(auto_bolt.detail['lapsed_at'])}ms</b><span>EXACT LAPSE</span></div>
          <div class="metric"><b>{_fmt(auto_bolt.closed_at)}ms</b><span>NOTICE</span></div>
          <div class="metric"><b>{_fmt(auto_lag)}ms</b><span>NOTICE LAG</span></div>
        </div>"""

    idle_metrics = ""
    if idle_bolt is not None:
        idle_metrics = f"""
        <div class="metrics">
          <div class="metric"><b>{_fmt(COARSE_TICK_MS)}ms</b><span>COARSE TICK</span></div>
          <div class="metric"><b>{_fmt(idle_bolt.detail['lapsed_at'])}ms</b><span>EXACT LAPSE</span></div>
          <div class="metric"><b>{_fmt(idle_bolt.closed_at)}ms</b><span>NOTICE (IDLE CUTOFF)</span></div>
          <div class="metric"><b>{_fmt(idle_lag)}ms</b><span>NOTICE LAG</span></div>
        </div>"""

    auto_report = render_html(auto_result, title="50ms window · auto 5ms tick")
    idle_report = render_html(idle_result, title="coarse tick · idle cutoff")
    auto_frame = (
        f'<iframe title="auto tick report" '
        f'srcdoc="{html.escape(auto_report, quote=True)}"></iframe>'
    )
    idle_frame = (
        f'<iframe title="idle cutoff report" '
        f'srcdoc="{html.escape(idle_report, quote=True)}"></iframe>'
    )

    page = f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>MO tick granularity — measured</title>
<style>{CSS}</style></head>
<body><main>
<h1>MO tick granularity — measured</h1>
<p class="sub">§8 open question: “start 100ms, measure”. The tick period is the
BOLT notice-lag budget, so it is derived from the tightest declared window.</p>
{banner}
{metrics}
<h2>DECISION</h2>
<pre>{_esc(POLICY_CODE)}</pre>
<p class="note">100ms stays the ceiling for ordinary multi-second windows; a
tight window gets a proportional tick. The poll shrinks with the tick so the
requested interval is actually observable, and the cost is one poll per half
tick.</p>
<h2>POLICY — AUTO TICK BY TIGHTEST WINDOW</h2>
{policy_html}
<h2>REAL TAILER — TICK GAPS ON THE POLL GRID</h2>
{gaps_html}
<p class="note">Measured by running <code>tail_events</code> on an empty file
with a virtual clock; each gap equals the effective period.</p>
<h2>50ms WINDOW — BOLT ON AN AUTO 5ms TICK</h2>
{auto_metrics}
<pre>{_esc(auto_jsonl.strip())}</pre>
{auto_frame}
<h2>IDLE CUTOFF — FINAL TICK KEEPS THE LAPSE</h2>
<pre>{_esc(IDLE_CODE)}</pre>
{idle_metrics}
<pre>{_esc(idle_jsonl.strip())}</pre>
{idle_frame}
<footer>
The judge still learns time only from <code>ev.ts</code>: the wall clock is
read by the tailer to mint ticks, exactly as premortem #6 requires. Regenerate
with <code>python3 -m mo.examples.tick_granularity_demo --out tick.html</code>;
exit code 1 means a measured claim is false.
</footer>
</main></body></html>
"""
    stats = {
        "policy_ok": policy_ok,
        "gaps_ok": gaps_ok,
        "auto_ok": auto_ok,
        "idle_ok": idle_ok,
        "auto_lag_ms": auto_lag,
        "idle_lag_ms": idle_lag,
        "worst_pct": worst_pct,
    }
    return page, ok, stats


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", help="write the HTML proof page here")
    args = parser.parse_args(argv)

    page, ok, stats = render()
    if args.out:
        Path(args.out).write_text(page, encoding="utf-8")
        print(f"wrote {args.out}", file=sys.stderr)
    else:
        print(page)
    print(
        f"policy={stats['policy_ok']} gaps={stats['gaps_ok']} "
        f"auto={stats['auto_ok']} idle={stats['idle_ok']} "
        f"worst_lag_pct={stats['worst_pct']:.1f} "
        f"auto_lag_ms={stats['auto_lag_ms']} "
        f"idle_lag_ms={stats['idle_lag_ms']} "
        f"result={'MEASURED' if ok else 'UNPROVEN'}",
        file=sys.stderr,
    )
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
