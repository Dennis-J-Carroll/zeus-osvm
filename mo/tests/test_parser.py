import pytest

from mo.parser import parse, ParseError
from mo.events import ZeusEvent


def test_parses_trigger_block():
    spec = parse(
        "on tool_declared as $tool:\n"
        "    EXPECT tool_called for $tool\n"
        "    WINDOW 5000\n"
    )
    assert len(spec.triggers) == 1
    t = spec.triggers[0]
    assert t.on_primitive == "tool_declared"
    assert t.expects == "tool_called"
    assert t.window_ms == 5000
    assert t.spec_line == 1


def test_parses_membership_assert():
    spec = parse("ASSERT no tool_called where identifier not in tool_declared\n")
    assert len(spec.assertions) == 1
    a = spec.assertions[0]
    assert a.spec_line == 1
    # violated by an undeclared call...
    seen = {"tool_declared": {"search"}}
    assert a.violated_by(ZeusEvent("tool_called", "rm_rf"), seen) is True
    # ...not by a declared one
    assert a.violated_by(ZeusEvent("tool_called", "search"), seen) is False
    # ...not by an unrelated primitive
    assert a.violated_by(ZeusEvent("send", "rm_rf"), seen) is False


def test_comments_and_blank_lines_ignored():
    spec = parse(
        "; every declared tool must be honored\n"
        "\n"
        "on tool_declared as $tool:   ; OBSERVE\n"
        "    EXPECT tool_called for $tool\n"
        "    WINDOW 5000\n"
        "\n"
        "ASSERT no tool_called where identifier not in tool_declared  ; safety\n"
    )
    assert len(spec.triggers) == 1
    assert len(spec.assertions) == 1


def test_window_missing_is_parse_error_with_line():
    with pytest.raises(ParseError) as exc:
        parse(
            "on tool_declared as $tool:\n"
            "    EXPECT tool_called for $tool\n"
            "ASSERT no tool_called where identifier not in tool_declared\n"
        )
    assert "WINDOW" in str(exc.value)


def test_binding_mismatch_is_parse_error():
    with pytest.raises(ParseError) as exc:
        parse(
            "on tool_declared as $tool:\n"
            "    EXPECT tool_called for $other\n"
            "    WINDOW 5000\n"
        )
    assert "line 2" in str(exc.value)


def test_unknown_directive_is_parse_error_with_line():
    with pytest.raises(ParseError) as exc:
        parse("FROBNICATE everything\n")
    assert "line 1" in str(exc.value)
