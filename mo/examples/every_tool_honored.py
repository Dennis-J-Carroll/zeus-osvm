"""M0 example spec (hand-built rules; M1 replaces this with .zspec text).

Intent — the worked example from the plan:

    on tool_declared as $tool:
        EXPECT tool_called for $tool
        WINDOW 5000

    ASSERT no tool_called where $tool not in declared_tools   ; -> CONDEMNED

Every declared tool must actually be called within 5s, and no tool may be
called that was never declared.
"""
from mo.rules import Spec, TriggerRule, AssertRule


def _undeclared_call(ev, seen):
    return (ev.primitive == "tool_called"
            and ev.identifier not in seen.get("tool_declared", set()))


spec = Spec(
    triggers=[
        TriggerRule(on_primitive="tool_declared", expects="tool_called",
                    window_ms=5000, spec_line=2),
    ],
    assertions=[
        AssertRule(spec_line=6, predicate=_undeclared_call,
                   detail={"rule": "tool_called for an undeclared tool"}),
    ],
)
