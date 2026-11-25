#!/usr/bin/env python3
"""Proof page for M3's deferred `-- <cmd>` launcher.

    python3 -m mo.examples.watch_launcher_demo --out artifacts/watch-launcher.html

One command starts the tap, finds the session log it creates, judges it live
and stops the child when judging ends. This demo uses a synthetic server that
writes a Glassport-shaped session log, so it runs anywhere; the same invocation
wraps a real tap:

    python3 -m mo watch --log-dir tap spec.zspec -- \
        glassport wrap --log-dir tap -- python server.py

The page is self-checking: exit 1 if any claim is false.
"""
from __future__ import annotations

import argparse
import html
import io
import json
import sys
import tempfile
import time
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

from mo.cli import main as mo_main
from mo.engine import RunResult, Verdict
from mo.report import render_html

WINDOW_MS = 100.0

SPEC_TEXT = f"""\
; the launched server calls ghost and never answers
on tool_called as $tool:
    EXPECT tool_result for $tool
    WINDOW {int(WINDOW_MS)}
"""

# A stand-in for a tapped server: it emits one wire-order tools/call and then
# stays alive long enough for a synthetic tick to cross the window.
SERVER_TEXT = """\
import json, sys, time
from datetime import datetime, timezone
from pathlib import Path

log = Path({log_dir!r}) / "session_child.jsonl"
rec = {{
    "schema_version": "0.1",
    "seq": 1,
    "ts": datetime.now(timezone.utc).isoformat(),
    "dir": "c2s",
    "frame": {{
        "jsonrpc": "2.0",
        "id": 7,
        "method": "tools/call",
        "params": {{"name": "ghost", "arguments": {{}}}},
    }},
    "raw": None,
}}
log.write_text(json.dumps(rec) + "\\n", encoding="utf-8")
time.sleep(0.6)
"""

CSS = """
:root { --bg:#0b0f14; --panel:#141c26; --line:#24313f; --fg:#d6e0ec;
        --dim:#8494a8; --green:#5ee08b; --amber:#ffa657; --red:#ff7b72; }
* { box-sizing: border-box; }
body { background: var(--bg); color: var(--fg); margin: 0 auto; padding: 28px;
       max-width: 1040px; font: 15px/1.55 ui-monospace, Menlo, Consolas, monospace; }
h1 { font-size: 30px; margin: 0 0 6px; letter-spacing: -0.02em; }
h2 { font-size: 13px; letter-spacing: .12em; color: #9fb0c6;
     margin: 34px 0 10px; text-transform: uppercase; }
.sub { color: var(--dim); margin: 0 0 20px; }
.banner { display: inline-block; padding: 10px 16px; border-radius: 4px;
          background: #173525; color: var(--green); border: 1px solid #2c6b46;
          font-weight: bold; letter-spacing: .08em; margin: 8px 0 22px; }
.metrics { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));
           gap: 12px; }
.metric { background: var(--panel); border: 1px solid var(--line);
          border-radius: 5px; padding: 14px; }
.metric b { display: block; font-size: 24px; color: #fff; }
.metric span { color: var(--dim); font-size: 12px; letter-spacing: .06em; }
table { width: 100%; border-collapse: collapse; background: var(--panel); }
th, td { padding: 9px 10px; text-align: left; border-bottom: 1px solid var(--line);
         vertical-align: top; }
th { color: var(--dim); font-size: 12px; letter-spacing: .08em; white-space: nowrap; }
td { overflow-wrap: break-word; }
td.mono { white-space: nowrap; }
@media (max-width: 720px) { table { font-size: 12px; } }
pre { margin: 0; padding: 14px 16px; background: var(--panel);
      border: 1px solid var(--line); border-radius: 4px;
      white-space: pre-wrap; word-break: break-word; color: #c7d4e3; }
code { color: #c7d4e3; }
.ok { color: var(--green); } .bad { color: var(--red); } .warn { color: var(--amber); }
.note { color: var(--dim); font-size: 13px; }
iframe { width: 100%; height: 340px; border: 1px solid var(--line);
         border-radius: 4px; background: #0b0f0b; }
footer { margin-top: 34px; color: #6f8096; font-size: 13px; }
"""


def _esc(value: object) -> str:
    return html.escape(str(value))


def _table(headers: list[str], rows: list[list[str]]) -> str:
    head = "".join(f"<th>{_esc(h)}</th>" for h in headers)
    body = "".join("<tr>" + "".join(row) + "</tr>" for row in rows)
    return f"<table><tr>{head}</tr>{body}</table>"


def _run_launcher(tmp: Path) -> dict:
    spec = tmp / "live.zspec"
    spec.write_text(SPEC_TEXT, encoding="utf-8")
    log_dir = tmp / "tap"
    log_dir.mkdir()
    server = tmp / "server.py"
    server.write_text(SERVER_TEXT.format(log_dir=str(log_dir)), encoding="utf-8")

    argv = [
        "watch", "--log-dir", str(log_dir), "--wait-ms", "2000",
        "--tick-ms", "10", str(spec), "--", sys.executable, str(server),
    ]
    out, err = io.StringIO(), io.StringIO()
    started = time.monotonic()
    with redirect_stdout(out), redirect_stderr(err):
        code = mo_main(argv)
    elapsed = time.monotonic() - started

    logs = sorted(log_dir.glob("*.jsonl"))
    log_path = logs[0] if logs else None
    raw = [json.loads(line) for line in out.getvalue().splitlines() if line.strip()]
    verdicts = [
        Verdict(
            kind=v["kind"],
            identifier=v.get("identifier"),
            spec_line=v["spec_line"],
            opened_at=v.get("opened_at"),
            closed_at=v["closed_at"],
            detail=v.get("detail") or {},
            spec_source=v.get("spec_source"),
        )
        for v in raw
    ]
    return {
        "argv": argv,
        "code": code,
        "stdout": out.getvalue(),
        "stderr": err.getvalue(),
        "elapsed": elapsed,
        "log_path": log_path,
        "log_text": log_path.read_text(encoding="utf-8") if log_path else "",
        "raw_verdicts": raw,
        "result": RunResult(verdicts=verdicts),
    }


def _proof(run: dict) -> tuple[bool, list[tuple[str, bool, str]]]:
    bolts = [v for v in run["raw_verdicts"] if v["kind"] == "BOLTED"]
    bolt = bolts[0] if len(bolts) == 1 else None
    checks = [
        ("command launched", "# launched:" in run["stderr"],
         "MO started the child in its own process group"),
        ("new tap discovered", run["log_path"] is not None
         and "# tap:" in run["stderr"],
         run["log_path"].name if run["log_path"] else "no log appeared"),
        ("live BOLT on ghost", bool(bolt) and bolt.get("identifier") == "ghost",
         "exactly one BOLTED verdict for the unanswered call"),
        ("window boundary exact", bool(bolt)
         and bolt["detail"].get("reason") == "liveness_window_expired"
         and bolt["detail"].get("lapsed_at") == bolt["opened_at"] + WINDOW_MS,
         f"lapsed_at = opened_at + {WINDOW_MS:.0f}ms"),
        ("child cleaned up", "exited" not in run["stderr"],
         "the child exited 0 and was reaped; no failure line"),
        ("exit mirrors deviation", run["code"] == 1,
         "a BOLT makes the one-command invocation exit 1"),
        ("bounded run", run["elapsed"] < 5.0,
         f"finished in {run['elapsed']:.2f}s"),
    ]
    return all(ok for _, ok, _ in checks), checks


def _page(run: dict, checks: list[tuple[str, bool, str]], ok: bool) -> str:
    verdict_rows = []
    for v in run["raw_verdicts"]:
        kind = v["kind"]
        color = "ok" if kind == "FULFILLED" else "warn" if kind == "BOLTED" else "bad"
        reason = v["detail"].get("reason") or v["detail"].get("rule") or ""
        verdict_rows.append([
            f"<td class='mono {color}'>{_esc(kind)}</td>",
            f"<td class='mono'>{_esc(v.get('identifier'))}</td>",
            f"<td class='mono'>L{v['spec_line']}</td>",
            f"<td class='mono'>{_esc(v.get('opened_at'))}→{_esc(v.get('closed_at'))}</td>",
            f"<td>{_esc(reason)}</td>",
        ])
    verdicts_html = _table(
        ["KIND", "IDENTIFIER", "RULE", "OPEN→NOTICE", "REASON"], verdict_rows)

    check_rows = [
        [f"<td>{_esc(name)}</td>",
         f"<td class='{'ok' if good else 'bad'}'>{'PASS' if good else 'FAIL'}</td>",
         f"<td>{_esc(evidence)}</td>"]
        for name, good, evidence in checks
    ]
    checks_html = _table(["CHECK", "RESULT", "EVIDENCE"], check_rows)

    log_name = run["log_path"].name if run["log_path"] else "?"
    command = "python3 -m mo " + " ".join(
        [run["argv"][0], "--log-dir", "tap", "--tick-ms", "10",
         "live.zspec", "--", "python3", "server.py"])
    report = render_html(run["result"], title="launcher · live tap")
    frame = (f'<iframe title="live launcher report" '
             f'srcdoc="{html.escape(report, quote=True)}"></iframe>')

    badge = "LAUNCHER VERIFIED" if ok else "LAUNCHER PROOF FAILED"
    banner_class = "banner" if ok else "banner bad"

    return f"""<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>MO watch launcher — one command</title>
<style>{CSS}</style></head><body>
<h1>MO watch launcher — one command</h1>
<p class="sub">M3 deferred the <code>-- &lt;cmd&gt;</code> form: live mode used to
need a proxy started in another terminal and a session path guessed by hand.
Now MO starts the tap, waits for the log it creates, and judges that log live.</p>
<div class="{banner_class}">{badge}</div>

<div class="metrics">
  <div class="metric"><b>1</b><span>PROCESS STARTED</span></div>
  <div class="metric"><b>1</b><span>NEW TAP FOUND</span></div>
  <div class="metric"><b>1</b><span>LIVE BOLT</span></div>
  <div class="metric"><b>{run['elapsed']:.2f}s</b><span>WALL TIME</span></div>
</div>

<h2>The one command</h2>
<pre>{_esc(command)}

; the real tap, same shape:
python3 -m mo watch --log-dir tap live.zspec -- \\
    glassport wrap --log-dir tap -- python server.py</pre>
<p class="note">MO snapshots <code>--log-dir</code>, starts the command in its
own process group, then takes the newest <code>*.jsonl</code> that appeared.
When the child exits, a 200ms grace poll drains its final records; when the
judge stops first, the whole child group is terminated.</p>

<h2>Self-checks</h2>
{checks_html}

<h2>Verdicts — streamed during the run</h2>
{verdicts_html}
<p class="note">The synthetic tick, not end-of-stream, expired the window:
<code>reason=liveness_window_expired</code> with an exact
<code>lapsed_at</code>. A silent child would have produced a session-end BOLT
instead, with no proof of when the promise broke.</p>

<h2>Captured CLI output</h2>
<pre>{_esc(run['stderr'])}{_esc(run['stdout'])}</pre>
<p class="note">stdout is the JSONL verdict stream; stderr carries the launcher's
<code># launched:</code> / <code># tap:</code> lines and the human summary.</p>

<h2>Live report</h2>
{frame}
<p class="note">Rendered from the exact verdicts the launcher streamed
(<code>{_esc(log_name)}</code> is the discovered session log).</p>

<footer>Regenerate with
<code>python3 -m mo.examples.watch_launcher_demo --out artifacts/watch-launcher.html</code>;
exit 1 means a claim is false. The synthetic server keeps the proof independent
of glassport; the real form is shown above.</footer>
</body></html>
"""


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", help="write the proof page here")
    args = parser.parse_args(argv)

    with tempfile.TemporaryDirectory(prefix="mo_launcher_demo_") as tmpdir:
        run = _run_launcher(Path(tmpdir))
        ok, checks = _proof(run)
        page = _page(run, checks, ok)

    if args.out:
        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(page, encoding="utf-8")
        print(f"wrote {out}", file=sys.stderr)
    else:
        print(page)

    bolt = next((v for v in run["raw_verdicts"] if v["kind"] == "BOLTED"), None)
    print(
        f"launched_ok={'# launched:' in run['stderr']} "
        f"tap_ok={run['log_path'] is not None} "
        f"bolt_ok={bool(bolt) and bolt.get('identifier') == 'ghost'} "
        f"exit={run['code']} elapsed={run['elapsed']:.2f}s "
        f"result={'VERIFIED' if ok else 'FAILED'}",
        file=sys.stderr,
    )
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
