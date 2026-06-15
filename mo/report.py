"""Render a RunResult: machine JSONL (one verdict per line), human summary,
and a self-contained HTML timeline (M4).

FULFILLED is logged, not just deviations — it buys coverage stats for free
(open question §8). The summary always prints BOLTED/CONDEMNED counts even at
zero so a clean session reads as an explicit pass, not silence.
"""
from __future__ import annotations

import html
from dataclasses import asdict

from .engine import RunResult, Verdict

KINDS = ("FULFILLED", "BOLTED", "CONDEMNED")

# CSS class per verdict kind — the green/amber/red of the §6 M4 sketch.
_KIND_CLASS = {"FULFILLED": "fulfilled", "BOLTED": "bolted",
               "CONDEMNED": "condemned"}


def verdict_to_dict(v: Verdict) -> dict:
    return asdict(v)


def jsonl_lines(result: RunResult):
    import json
    for v in result.verdicts:
        yield json.dumps(verdict_to_dict(v))


def human_summary(result: RunResult) -> str:
    counts = {k: 0 for k in KINDS}
    for v in result.verdicts:
        counts[v.kind] = counts.get(v.kind, 0) + 1

    lines = ["MO verdict summary", "=================="]
    for k in KINDS:
        lines.append(f"  {k}: {counts[k]}")
    lines.append(
        f"  coverage: {result.unmatched_events}/{result.total_events} "
        f"events matched no rule"
    )
    return "\n".join(lines)


_CSS = """
:root { --bg:#0b0f0b; --panel:#111911; --line:#1f2d1f; --fg:#c9e4c9;
        --dim:#739173; --green:#4ade80; --amber:#fb923c; --red:#f87171; }
* { box-sizing: border-box; }
body { background: var(--bg); color: var(--fg); margin: 0 auto; padding: 1rem;
       max-width: 64rem; font: 14px/1.5 ui-monospace, Menlo, Consolas, monospace; }
h1 { color: var(--green); font-size: 1.2rem; margin: 0 0 .25rem; }
.dim { color: var(--dim); }
.banner { display: inline-block; padding: .2rem .7rem; border-radius: .25rem;
          font-weight: bold; margin: .5rem 0 1rem; }
.ok { color: #052e16; background: var(--green); }
.bad { color: #2b0a0a; background: var(--red); }
.counts span { margin-right: 1rem; }
.fulfilled { color: var(--green); } .bolted { color: var(--amber); }
.condemned { color: var(--red); }
.row { display: flex; align-items: center; gap: .75rem; padding: .2rem 0;
       border-top: 1px solid var(--line); }
.label { flex: 0 0 16rem; white-space: nowrap; overflow: hidden;
         text-overflow: ellipsis; }
.track { position: relative; flex: 1; height: 1.1rem;
         background: var(--panel); border-radius: .2rem; }
.bar { position: absolute; top: 0; height: 100%; min-width: 2px;
       border-radius: .2rem; }
.bar.fulfilled { background: var(--green); }
.bar.bolted { background: var(--amber); }
.bar.condemned { width: 0; border-left: 3px solid var(--red); }
.axis { display: flex; justify-content: space-between; color: var(--dim);
        font-size: .8rem; margin: .25rem 0 .5rem 16.75rem; }
"""


def _span_geometry(opened_at, closed_at, t0: float, t1: float):
    """Place one verdict on the time axis -> (left%, width%).

    Linear time scale: x is proportional to real elapsed ms, so a BOLT's
    five-second lapse genuinely looks wide and clustered calls sit close.
    A CONDEMNED verdict has no opening — it is a point marker at its instant
    (width 0). Spans get a small floor width so a zero-duration span is still
    visible.
    """
    span = (t1 - t0) or 1.0
    if opened_at is None:                       # CONDEMNED: a point in time
        return (closed_at - t0) / span * 100.0, 0.0
    left = (opened_at - t0) / span * 100.0
    width = max((closed_at - opened_at) / span * 100.0, 0.6)
    return left, width


def render_html(result: RunResult, title: str = "MO session") -> str:
    """A self-contained, no-JS, offline HTML timeline of a judged session.

    Every identifier came off the wire and is escaped before it touches the
    page — a hostile server can name a tool '<img onerror=...>' and this report
    opens from file:// where injected script would run with local reach.
    """
    verdicts = result.verdicts
    deviations = sum(1 for v in verdicts if v.kind in ("BOLTED", "CONDEMNED"))
    banner = ('<span class="banner ok">CLEAN — no deviations</span>'
              if not deviations else
              f'<span class="banner bad">DEVIATIONS FOUND — {deviations}'
              f'</span>')

    counts = {k: sum(1 for v in verdicts if v.kind == k) for k in KINDS}
    counts_html = "".join(
        f'<span class="{_KIND_CLASS[k]}">{k}: {counts[k]}</span>'
        for k in KINDS)

    stamps = [v.opened_at for v in verdicts if v.opened_at is not None]
    stamps += [v.closed_at for v in verdicts if v.closed_at is not None]
    t0, t1 = (min(stamps), max(stamps)) if stamps else (0.0, 1.0)

    rows = []
    for v in verdicts:
        cls = _KIND_CLASS[v.kind]
        ident = html.escape(str(v.identifier))
        left, width = _span_geometry(v.opened_at, v.closed_at, t0, t1)
        detail = v.detail.get("reason") or v.detail.get("rule") or ""
        tip = html.escape(f"{v.kind} {ident} @line {v.spec_line} {detail}".strip())
        rows.append(
            f'<div class="row"><div class="label {cls}">'
            f'{v.kind} <span class="dim">·</span> {ident} '
            f'<span class="dim">L{v.spec_line}</span></div>'
            f'<div class="track"><div class="bar {cls}" title="{tip}" '
            f'style="left:{left:.2f}%; width:{width:.2f}%;"></div></div></div>')

    return f"""<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(title)}</title><style>{_CSS}</style></head>
<body>
<h1>MO — {html.escape(title)}</h1>
<div>{banner}</div>
<div class="counts">{counts_html}
<span class="dim">coverage: {result.unmatched_events}/{result.total_events}
 events matched no rule</span></div>
<div class="axis"><span>{t0:.0f} ms</span><span>{t1:.0f} ms</span></div>
{''.join(rows)}
</body></html>"""
