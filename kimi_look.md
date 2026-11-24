Kimi Look


 1. Real live tap, end-to-end. ✅ DONE — ran glassport wrap -- python3 fake_server.py
    and mo watch mo/examples/live_liveness.zspec on the growing session log.
    One FULFILLED (web_search, correlation 3), zero BOLTs/CONDEMNEDs.
    Report generated at /tmp/zeus_live_dogfood_report.html.


 2. Identifier-collision test (premortem #2). ✅ DONE — added correlation ids to
    ZeusEvent/Obligation and wired them through both MCP adapters. Two concurrent
    tool_called search with different jsonrpc ids no longer cross-close; the
    matching result closes its own obligation. Regression tests in
    test_engine.py and test_adapter_mcp.py.

 3. Malformed input. ✅ DONE — tailer now skips truncated JSONL lines instead of
    crashing; projector survives tools/call frames missing params, name, or id.
    Tests added for truncated JSONL, malformed calls, and orphaned results.

 4. .zspec parser errors — already has line N: errors (M1); ✅ DONE — mo
    watch/report/eval now surface parse errors cleanly (exit 2, message on
    stderr) instead of dumping a traceback.


 Next — ideal suggested steps

 1. Wire-order fabrication ASSERT. ✅ DONE — added `GRACE <ms>` to the
    membership ASSERT. A call opens a pending check, a declaration of the same
    identifier inside the window resolves it, and a tick/later event/end-of-
    stream past the deadline condemns it. The pre-delivery gate escalates a
    pending check rather than allowing an effect it cannot clear. Demo:
    `mo/examples/late_declaration.zspec` + `late_declaration_session.jsonl`.
 2. Persist a reproducible live-dogfood script. ✅ DONE —
    `mo/examples/live_dogfood.py` is a self-contained fake MCP server +
    orchestrator; `python3 -m mo.examples.live_dogfood` is the one-command demo,
    with `mo/tests/test_live_dogfood.py` as its CI guard.
 3. Dense-report readability (M4 known limitation). ✅ DONE — `_span_geometry`
    now uses even lanes in verdict order, so millisecond calls stay visible
    beside a six-second BOLT; true durations are in the bar tooltips. Demo:
    `mo/examples/dense_liveness.zspec` + `dense_session.jsonl`; pinned by
    `test_dense_verdicts_use_even_lanes_and_tooltip_durations`.
 4. Real-failure live demo. ✅ DONE — `mo/examples/real_failure_demo.py` drives
    the real `glassport probe` against `unhonored_mcp_server.py`, tails the
    live tap with `mo watch`, and asserts exactly one BOLT (`report_export`,
    `liveness_window_expired`) while glassport is still running. Spec:
    `mo/examples/real_failure.zspec`; requires `pip install glassport` or
    `GLASSPORT_BIN`.
 5. Determinism lint rule (premortem #6). ✅ DONE — `mo/determinism.py` scans
    the judge core (`engine`, `events`, `gate`, `ledger`, `parser`, `rules`) for
    any `time`/`datetime`/`timeit` import or call, aliases included.
    `mo/tests/test_determinism.py` fails the suite on a clock read; the proof
    page is `python3 -m mo.examples.determinism_guard_demo --out proof.html`.
 6. Exact BOLT lapse-time (M0 deferred decision). ✅ DONE — a lapsed BOLT now
    carries `detail.lapsed_at` (the window boundary) and
    `detail.notice_lag_ms`; `closed_at` stays notice-time for compatibility,
    and report tooltips show the declared window, notice duration, lapse
    instant and lag. Demo: `mo/examples/lapse_time_demo.py`.
 7. Declaration→execution correlation. ✅ DONE — correlation matching is now
    scoped to the obligation's own instance id. A `tool_declared` obligation
    (no id) accepts the first matching `tool_called` even though the call
    carries a jsonrpc id; previously every correlated call was vetoed by its
    own declaration and `every_tool_honored.zspec` BOLTED honored tools.
    Concurrent same-tool calls still pair only with their own result. Demo:
    `mo/examples/declaration_correlation_demo.py`.


 Next — remaining candidates

 - Tick granularity for live mode. ✅ DONE — measured with the real tailer on a
   virtual clock: the effective tick period is the BOLT notice-lag budget, so
   `mo watch` keeps a 100ms ceiling but tightens to one tenth of the tightest
   declared window (5ms floor) and polls at half the tick. Measured worst lag
   is 10% of the window; an idle return emits a final tick so a lapse at the
   cutoff is reported as `liveness_window_expired`, not a session end.
   Demo: `python3 -m mo.examples.tick_granularity_demo`; `--tick-ms` overrides.
 - Exact BOLT lapse-time. ✅ DONE (see item 6 above).
 - Run the MO suite in CI. ✅ DONE — `.github/workflows/ci.yml` now has a
   separate `mo` job (Python 3.12, `pip install pytest`, `pytest mo/ -q`)
   beside the unchanged C `make check` job. `mo/tests/test_ci_gate.py` pins
   both jobs, and `mo/examples/ci_gate_demo.py` renders a proof page.
 - Composable spec fragments (§8's last open question). ✅ DONE —
   `INCLUDE <path>` composes `.zspec` files relative to the including file,
   include-once for diamonds, with cycles and missing files reported as
   `file: line N` errors. `spec_source` rides every rule, obligation and
   verdict, so a BOLT names the fragment it came from; single-file JSONL is
   unchanged. Demo: `python3 -m mo.examples.spec_fragments_demo`.
