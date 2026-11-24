"""Prove composable .zspec fragments and per-file line provenance.

    python3 -m mo.examples.spec_fragments_demo --out fragments.html

§8's last open question was "one spec per server, or composable spec
fragments?". The decision implemented here: composable fragments. A root spec
names shared files with `INCLUDE <path>`; the file is parsed at that point,
relative to the including file, into the same rule set. Inclusion is
include-once, so a diamond (two fragments sharing one base) adds one copy of
the shared rules rather than two obligations per event. A cycle or a missing
file is a parse error carrying the including file and line.

Fragments are only usable if a verdict still says *which* file a line came
from, so every rule, obligation and verdict carries `spec_source`. Single-file
specs keep the old JSONL shape and the compact `L<n>` report label.

The page drives the real parser and judge. Exit code is 1 if any claim fails.
"""
from __future__ import annotations

import argparse
import html
import json
import sys
import tempfile
from pathlib import Path

from mo.engine import run
from mo.events import ZeusEvent
from mo.parser import ParseError, parse_file
from mo.report import jsonl_lines, render_html

FRAGMENT = """\
; shared MCP liveness rules: every declared tool must be called
on tool_declared as $tool:
    EXPECT tool_called for $tool
    WINDOW 5000
"""

ROOT = """\
; per-server root: compose the shared fragment, then add the local safety rule
INCLUDE fragments/mcp_liveness.zspec

ASSERT no tool_called where identifier not in tool_declared
"""

COMMON = """\
; shared by both feature fragments below
on tool_called as $tool:
    EXPECT tool_result for $tool
    WINDOW 250
"""

BAD_FRAGMENT = "FROBNICATE everything\n"

EVENTS = [
    ZeusEvent("tool_declared", "search", ts=0.0),
    ZeusEvent("tool_called", "search", ts=400.0),
    ZeusEvent("tool_declared", "report_export", ts=1000.0),
]

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
.specs { display: grid; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr));
         gap: 14px; }
table { width: 100%; border-collapse: collapse; background: #151c25; }
th, td { padding: 8px 10px; text-align: left; border-bottom: 1px solid #222d3a; }
th { white-space: nowrap; }
td { overflow-wrap: break-word; }
td.kind, td.source, td.ident { white-space: nowrap; }
th { color: #7d8799; font-size: 12px; letter-spacing: .08em; }
td.num { text-align: right; }
@media (max-width: 720px) {
  /* keep the proof tables on one line in a narrow Browser panel */
  table { font-size: 12px; }
}
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
  width: 100%; height: 360px; border: 1px solid #263241;
  border-radius: 4px; background: #0b0f0b;
}
footer { margin-top: 34px; color: #6d7787; font-size: 13px; }
"""


def _esc(value: object) -> str:
    return html.escape(str(value))


def _write(path: Path, text: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


def _table(headers: list[str], rows: list[list[str]]) -> str:
    head = "".join(f"<th>{_esc(h)}</th>" for h in headers)
    # row cells are already full <td> markup (some carry a status class)
    body = "".join(
        "<tr>" + "".join(row) + "</tr>"
        for row in rows
    )
    return f"<table><tr>{head}</tr>{body}</table>"


def _basename(path: str | None) -> str:
    return Path(path).name if path else "?"


def _compose(tmp: Path):
    frag = _write(tmp / "fragments" / "mcp_liveness.zspec", FRAGMENT)
    root = _write(tmp / "server.zspec", ROOT)
    spec = parse_file(str(root))
    result = run(iter(EVENTS), spec)
    return root, frag, spec, result


def _diamond(tmp: Path):
    _write(tmp / "common.zspec", COMMON)
    _write(tmp / "feature_a.zspec", "INCLUDE common.zspec\n")
    _write(tmp / "feature_b.zspec", "INCLUDE common.zspec\n")
    root = _write(tmp / "diamond.zspec",
                  "INCLUDE feature_a.zspec\nINCLUDE feature_b.zspec\n")
    return root, parse_file(str(root))


def _cycle_error(tmp: Path) -> str:
    a = _write(tmp / "cycle_a.zspec", "INCLUDE cycle_b.zspec\n")
    _write(tmp / "cycle_b.zspec", "INCLUDE cycle_a.zspec\n")
    try:
        parse_file(str(a))
    except ParseError as exc:
        return str(exc)
    return "no error (unexpected)"


def _fragment_error(tmp: Path) -> str:
    _write(tmp / "bad_frag.zspec", BAD_FRAGMENT)
    root = _write(tmp / "bad_root.zspec", "INCLUDE bad_frag.zspec\n")
    try:
        parse_file(str(root))
    except ParseError as exc:
        return str(exc)
    return "no error (unexpected)"


def _rule_rows(spec) -> list[list[str]]:
    rows = []
    for trigger in spec.triggers:
        rows.append([
            f'<td class="source">{_esc(_basename(trigger.spec_source))}</td>',
            f"<td class='num'>L{trigger.spec_line}</td>",
            '<td class="kind">TRIGGER</td>',
            f"<td><code>on {_esc(trigger.on_primitive)} → "
            f"EXPECT {_esc(trigger.expects)}</code></td>",
            f"<td class='num'>{trigger.window_ms:g}ms</td>",
        ])
    for rule in spec.assertions:
        rows.append([
            f'<td class="source">{_esc(_basename(rule.spec_source))}</td>',
            f"<td class='num'>L{rule.spec_line}</td>",
            '<td class="kind">ASSERT</td>',
            f"<td><code>{_esc(rule.detail.get('rule', ''))}</code></td>",
            "<td class='num'>—</td>",
        ])
    return rows


def render() -> tuple[str, bool, dict]:
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = Path(tmpdir)
        root, frag, spec, result = _compose(tmp)
        diamond_root, diamond_spec = _diamond(tmp)
        cycle_message = _cycle_error(tmp)
        fragment_message = _fragment_error(tmp)

    source_ok = (
        len(spec.triggers) == 1
        and spec.triggers[0].spec_source == str(frag)
        and spec.triggers[0].spec_line == 2
        and len(spec.assertions) == 1
        and spec.assertions[0].spec_source == str(root)
        and spec.assertions[0].spec_line == 4
    )

    verdicts_ok = (
        [v.kind for v in result.verdicts] == ["FULFILLED", "BOLTED"]
        and all(v.spec_source == str(frag) and v.spec_line == 2
                for v in result.verdicts)
        and result.verdicts[1].identifier == "report_export"
    )

    diamond_ok = (
        len(diamond_spec.triggers) == 1
        and diamond_spec.triggers[0].spec_source.endswith("common.zspec")
    )

    cycle_ok = "include cycle" in cycle_message
    fragment_error_ok = ("bad_frag.zspec: line 1" in fragment_message
                         and "unknown directive 'FROBNICATE'" in fragment_message)

    ok = source_ok and verdicts_ok and diamond_ok and cycle_ok and fragment_error_ok

    banner = (
        '<div class="banner clean">SPEC FRAGMENTS — COMPOSED</div>'
        if ok else
        '<div class="banner dirty">SPEC FRAGMENTS — UNPROVEN</div>'
    )

    attributed = sum(1 for v in result.verdicts
                     if v.spec_source == str(frag))

    metrics = f"""
    <div class="metrics">
      <div class="metric"><b>2</b><span>FILES COMPOSED</span></div>
      <div class="metric"><b>{len(spec.triggers) + len(spec.assertions)}</b><span>RULES MERGED</span></div>
      <div class="metric"><b>{len(diamond_spec.triggers)}</b><span>DIAMOND COPIES</span></div>
      <div class="metric"><b>{attributed}/{len(result.verdicts)}</b><span>VERDICTS ATTRIBUTED</span></div>
    </div>"""

    specs_html = f"""
    <div class="specs">
      <div><p class="note">server.zspec — the root spec</p>
        <pre>{_esc(ROOT)}</pre></div>
      <div><p class="note">fragments/mcp_liveness.zspec — included here</p>
        <pre>{_esc(FRAGMENT)}</pre></div>
    </div>"""

    rules_html = _table(
        ["SOURCE", "LINE", "KIND", "RULE", "WINDOW"],
        _rule_rows(spec),
    )

    verdict_rows = []
    for v in result.verdicts:
        reason = v.detail.get("reason") or v.detail.get("rule") or ""
        verdict_rows.append([
            f"<td class='kind {'ok' if v.kind == 'FULFILLED' else 'bad'}'>"
            f"{v.kind}</td>",
            f'<td class="ident"><code>{_esc(v.identifier)}</code></td>',
            f'<td class="source">{_esc(_basename(v.spec_source))}</td>',
            f"<td class='num'>L{v.spec_line}</td>",
            f"<td>{_esc(reason)}</td>",
        ])
    verdicts_html = _table(
        ["KIND", "IDENTIFIER", "SOURCE", "LINE", "REASON"],
        verdict_rows,
    )

    jsonl = "\n".join(jsonl_lines(result))
    report = render_html(result, title="composed fragments · server.zspec")
    report_frame = (
        f'<iframe title="fragment report" '
        f'srcdoc="{html.escape(report, quote=True)}"></iframe>'
    )

    diamond_html = f"""
    <p class="note">diamond.zspec includes feature_a and feature_b; both include
    common.zspec. The shared rule lands once, so a call opens one obligation.</p>
    <pre>{_esc("INCLUDE feature_a.zspec\nINCLUDE feature_b.zspec")}</pre>
    <p class="note">{_esc(_basename(str(diamond_root)))} composes
    {len(diamond_spec.triggers)} trigger rule, sourced from
    {_esc(_basename(diamond_spec.triggers[0].spec_source))}.</p>"""

    page = f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>MO spec fragments — composed</title>
<style>{CSS}</style></head>
<body><main>
<h1>MO spec fragments — composed</h1>
<p class="sub">§8: “one spec per server, or composable spec fragments?” Fragments
win, and provenance survives composition: a verdict names the file its rule
came from.</p>
{banner}
{metrics}
<h2>THE TWO FILES</h2>
{specs_html}
<h2>COMPOSED RULE SET — ONE SPEC, TWO SOURCES</h2>
{rules_html}
<p class="note">The trigger came from the fragment at line 2; the assertion
came from the root at line 4. No line is ambiguous across files.</p>
<h2>VERDICTS — ATTRIBUTED TO THE FRAGMENT</h2>
{verdicts_html}
<pre>{_esc(jsonl)}</pre>
{report_frame}
<h2>DIAMOND INCLUDE — ONE COPY</h2>
{diamond_html}
<h2>ERROR PATHS</h2>
<pre>{_esc(cycle_message)}</pre>
<pre>{_esc(fragment_message)}</pre>
<p class="note">Both errors name the including file and line; a bad fragment
names itself.</p>
<footer>
Raw JSONL above keeps the whole path; the table shortens it to the basename.
A single-file spec still emits exactly the old five keys with no
<code>spec_source</code>. Regenerate with
<code>python3 -m mo.examples.spec_fragments_demo --out fragments.html</code>;
exit code 1 means a claim is false.
</footer>
</main></body></html>
"""
    stats = {
        "source_ok": source_ok,
        "verdicts_ok": verdicts_ok,
        "diamond_ok": diamond_ok,
        "cycle_ok": cycle_ok,
        "fragment_error_ok": fragment_error_ok,
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
        " ".join(f"{name}={value}" for name, value in stats.items())
        + f" result={'COMPOSED' if ok else 'UNPROVEN'}",
        file=sys.stderr,
    )
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
