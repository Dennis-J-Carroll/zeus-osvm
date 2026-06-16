# Golden fixtures — the adapter contract, frozen

These files pin the **shape** of what Glassport's `from_mcp_session()` hands MO.
MO's own 50-odd tests never import `glassport` (the seam, premortem #4), which
means a drift in Glassport's `InteractionTrace` shape — a renamed `metadata`
key, a moved `jsonrpc_id`, a different `parts` layout — would pass every MO test
and silently break every *real* verdict. Clean report, broken pipeline:
premortem #5 ("silence reads as a pass") one level up.

A frozen fixture is the trust boundary. `test_adapter_golden.py` feeds these
through the pure projection layer (`trace_to_events` / `frames_to_events`) and
asserts the exact ZeusEvent stream. No live `glassport` import — the fixture
*is* the captured contract.

## Files

- `mcp_session.golden.jsonl` — a real-shaped Glassport tap log (the §9
  fabrication session: `web_search` declared, `arxiv_lookup` called but never
  declared, both answered). Copied from `mo/examples/mcp_session.jsonl`; kept
  here so the contract test owns its input and example edits can't move it.
- `mcp_session.events.json` — the ZeusEvent stream the projector MUST produce
  from the above. THIS is the contract. Regenerate deliberately (see below)
  when — and only when — the adapter's projection is meant to change.

## Regenerating (a deliberate act, never automatic)

    python3 -m mo.tests.fixtures.regen

Review the diff. If it changed and you didn't mean it to, the adapter drifted —
that's the test doing its job. If you meant it, commit the new golden with a
message that says why the contract moved.
