"""Render a RunResult: machine JSONL (one verdict per line), human summary,
and a self-contained HTML timeline (M4).

FULFILLED is logged, not just deviations — it buys coverage stats for free
(open question §8). The summary always prints BOLTED/CONDEMNED counts even at
zero so a clean session reads as an explicit pass, not silence.
"""
from __future__ import annotations

import html
import os
from dataclasses import asdict

from .engine import RunResult, Verdict

KINDS = ("FULFILLED", "BOLTED", "CONDEMNED")

# CSS class per verdict kind — the green/amber/red of the §6 M4 sketch.
_KIND_CLASS = {"FULFILLED": "fulfilled", "BOLTED": "bolted",
               "CONDEMNED": "condemned"}


def verdict_to_dict(v: Verdict) -> dict:
    d = asdict(v)
    # Keep the single-file JSONL shape unchanged; only a composed fragment
    # carries a source, and then it is named explicitly.
    if d.get("spec_source") is None:
        d.pop("spec_source", None)
    return d


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


def _span_geometry(lane: int, lanes: int, point: bool = False):
    """Place one verdict in an even-width lane -> (left%, width%).

    Linear time made millisecond calls indistinguishable beside a multi-second
    BOLT. Lanes are ordinal (verdict order), so every verdict stays visible;
    the tooltip carries the true duration. CONDEMNED passes point=True so its
    CSS point marker is not turned into a lane-width bar.
    """
    lanes = max(lanes, 1)
    slot = 100.0 / lanes
    width = 0.0 if point else max(slot * 0.72, 0.6)
    return lane * slot, width


def render_html(result: RunResult, title: str = "MO session") -> str:
    """A self-contained, no-JS, offline HTML report of a judged session.

    Verdicts occupy even lanes in verdict order, not a linear time axis, so a
    dense burst stays legible beside a wide BOLT; true durations are in the
    bar tooltips. Every identifier came off the wire and is escaped before it
    touches the page — a hostile server can name a tool '<img onerror=...>' and
    this report opens from file:// where injected script would run with local
    reach.
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
    lanes = max(len(verdicts), 1)
    for lane, v in enumerate(verdicts):
        cls = _KIND_CLASS[v.kind]
        ident = html.escape(str(v.identifier))
        left, width = _span_geometry(lane, lanes, point=v.opened_at is None)
        if v.opened_at is None:
            timing = "instant"
        else:
            parts = []
            window = v.detail.get("window_ms")
            if v.kind == "BOLTED" and window is not None:
                # A BOLT closes at notice-time, not at the lapse instant, so
                # its honest span is the declared window; the notice lag is
                # reported separately instead of inflating the duration.
                parts.append(f"window={window:.0f}ms")
            parts.append(
                f"duration={max(v.closed_at - v.opened_at, 0.0):.0f}ms")
            if v.kind == "BOLTED" and v.detail.get("lapsed_at") is not None:
                parts.append(f"lapsed_at={v.detail['lapsed_at']:.0f}ms")
            if v.kind == "BOLTED" and v.detail.get("notice_lag_ms") is not None:
                parts.append(
                    f"notice_lag={v.detail['notice_lag_ms']:.0f}ms")
            timing = " ".join(parts)
        detail = v.detail.get("reason") or v.detail.get("rule") or ""
        # A composed fragment keeps its own file in the provenance; a plain
        # single-file spec keeps the old compact L<n> label.
        location = (f"L{v.spec_line}" if not v.spec_source
                    else f"{os.path.basename(v.spec_source)}:{v.spec_line}")
        origin = f"{v.spec_source}:" if v.spec_source else ""
        tip = html.escape(
            f"{v.kind} {ident} @line {origin}{v.spec_line} "
            f"{timing} {detail}".strip()
        )
        rows.append(
            f'<div class="row"><div class="label {cls}">'
            f'{v.kind} <span class="dim">·</span> {ident} '
            f'<span class="dim">{html.escape(location)}</span></div>'
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
<div class="dim">even lanes in verdict order · session {t0:.0f}–{t1:.0f} ms ·
durations, BOLT lapse and notice lag in tooltips</div>
<div class="axis"><span>lane 1</span><span>lane {lanes}</span></div>
{''.join(rows)}
</body></html>"""
