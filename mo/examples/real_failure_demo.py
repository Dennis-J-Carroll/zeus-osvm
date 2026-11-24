#!/usr/bin/env python3
"""Real-failure live demo: glassport tap + `mo watch` -> exactly one BOLT.

    python3 -m mo.examples.real_failure_demo --out artifacts/real-failure.html

Unlike `live_dogfood.py`, this does **not** synthesize the tap log. It starts
the real `glassport probe` against `unhonored_mcp_server.py`, tails the session
file glassport creates, and runs MO's live judge over it. The server declares
`report_export` and then never answers its `tools/call`; the demo asserts:

  * exactly one verdict, a BOLTED `report_export`;
  * the reason is `liveness_window_expired`, not `session_ended_unresolved`;
  * the BOLT was printed while the glassport process was still running;
  * the captured log is a real glassport header + tools/call.

Requires `glassport` on PATH or `GLASSPORT_BIN=/path/to/glassport`
(`pip install glassport`). The report is optional; pass `--out FILE`.
"""
from __future__ import annotations

import argparse
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from mo.cli import load_spec, watch
from mo.report import render_html

HERE = Path(__file__).resolve().parent
SPEC = HERE / "real_failure.zspec"
SERVER = HERE / "unhonored_mcp_server.py"


def find_glassport(explicit: str | None = None) -> str | None:
    if explicit:
        return explicit if os.access(explicit, os.X_OK) else None
    env = os.environ.get("GLASSPORT_BIN")
    if env and os.access(env, os.X_OK):
        return env
    return shutil.which("glassport")


class _LiveOut(io.StringIO):
    """Capture the streaming verdicts and note whether the tap was alive."""

    def __init__(self, probe: subprocess.Popen):
        super().__init__()
        self.probe = probe
        self.bolt_seen_at: float | None = None
        self.probe_alive_at_bolt: bool | None = None

    def write(self, text: str) -> int:
        if self.bolt_seen_at is None and '"kind": "BOLTED"' in text:
            self.bolt_seen_at = time.monotonic()
            self.probe_alive_at_bolt = self.probe.poll() is None
        return super().write(text)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--glassport", help="glassport executable")
    parser.add_argument("--out", help="write the live MO HTML report here")
    args = parser.parse_args(argv)

    glassport = find_glassport(args.glassport)
    if not glassport:
        print(
            "mo: real-failure demo needs glassport "
            "(`pip install glassport`, or set GLASSPORT_BIN)",
            file=sys.stderr,
        )
        return 2

    started = time.monotonic()
    probe: subprocess.Popen | None = None
    try:
        with tempfile.TemporaryDirectory(prefix="mo_real_failure_") as tmp:
            log_dir = Path(tmp) / "tap"
            log_dir.mkdir()

            probe_cmd = [
                glassport, "probe",
                "--log-dir", str(log_dir),
                "--timeout", "2",
                "--call", "report_export", "{}",
                "--", sys.executable, str(SERVER),
            ]
            print(f"# glassport: {' '.join(probe_cmd)}", file=sys.stderr)
            probe = subprocess.Popen(
                probe_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True,
            )

            log_path: Path | None = None
            deadline = time.monotonic() + 10.0
            while time.monotonic() < deadline:
                logs = sorted(log_dir.glob("*.jsonl"))
                if logs:
                    log_path = logs[0]
                    break
                if probe.poll() is not None:
                    break
                time.sleep(0.02)

            if log_path is None:
                print("# FAIL: glassport did not create a session log",
                      file=sys.stderr)
                return 1

            print(f"# tap: {log_path}", file=sys.stderr)
            live = _LiveOut(probe)
            result = watch(
                load_spec(str(SPEC)), str(log_path),
                tick_ms=50.0, idle_ms=3000.0, out=live,
            )
            sys.stdout.write(live.getvalue())

            probe_out, probe_err = probe.communicate(timeout=10.0)
            kinds = [v.kind for v in result.verdicts]
            bolts = [v for v in result.verdicts if v.kind == "BOLTED"]
            log_text = log_path.read_text(encoding="utf-8")

            real_tap = (
                '"type": "glassport.header"' in log_text
                and '"mode": "probe"' in log_text
                and '"method": "tools/call"' in log_text
                and '"name": "report_export"' in log_text
            )
            ok = (
                kinds == ["BOLTED"]
                and len(bolts) == 1
                and bolts[0].identifier == "report_export"
                and bolts[0].detail.get("reason") == "liveness_window_expired"
                and bolts[0].detail.get("correlation") is not None
                and live.probe_alive_at_bolt is True
                and real_tap
            )

            if args.out:
                out_path = Path(args.out)
                out_path.parent.mkdir(parents=True, exist_ok=True)
                out_path.write_text(
                    render_html(result, title="real failure · live glassport tap"),
                    encoding="utf-8",
                )
                print(f"# report: {out_path}", file=sys.stderr)

            if ok:
                print(
                    f"# PASS: exactly one BOLT on report_export, "
                    f"reason=liveness_window_expired, fired mid-session "
                    f"(probe rc={probe.returncode}, "
                    f"bolt_at=+{live.bolt_seen_at - started:.2f}s).",
                    file=sys.stderr,
                )
                return 0

            print(f"# FAIL: verdicts={kinds}, live_probe={live.probe_alive_at_bolt}, "
                  f"real_tap={real_tap}", file=sys.stderr)
            if probe_out:
                print(f"# glassport stdout:\n{probe_out}", file=sys.stderr)
            if probe_err:
                print(f"# glassport stderr:\n{probe_err}", file=sys.stderr)
            return 1
    except Exception as exc:                      # noqa: BLE001 - demo boundary
        print(f"# ERROR: {exc}", file=sys.stderr)
        return 1
    finally:
        if probe is not None and probe.poll() is None:
            probe.kill()
            probe.communicate()


if __name__ == "__main__":
    raise SystemExit(main())
