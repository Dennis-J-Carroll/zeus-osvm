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

 1. Wire-order fabrication ASSERT. In live mode declarations arrive after calls
    can already be in flight, so `ASSERT no tool_called where identifier not in
    tool_declared` is racy. Build an ASSERT variant that tolerates declarations
    arriving within a short window after the call (or declare the policy in the
    spec language).
 2. Persist a reproducible live-dogfood script. The manual run worked; turn the
    one-off into `mo/examples/live_dogfood.py` (self-contained fake MCP server +
    orchestrator) so the demo is runnable in one command.
 3. Dense-report readability (M4 known limitation). The linear time scale
    collapses fast calls next to a wide BOLT. Swap `_span_geometry` to even-lane
    spacing with true durations in tooltips once a real dense session proves it
    unreadable.
 4. Real-failure live demo. Run a server that declares a tool and never honors
    it through glassport + mo watch and confirm exactly one BOLT, mid-session.
