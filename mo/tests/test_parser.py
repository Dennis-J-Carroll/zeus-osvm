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


def test_parses_grace_membership_assert():
    spec = parse(
        "ASSERT no tool_called where identifier not in tool_declared GRACE 250\n"
    )
    a = spec.assertions[0]
    assert a.grace_ms == 250
    assert a.primitive_a == "tool_called"
    assert a.primitive_b == "tool_declared"
    assert a.detail["grace_ms"] == 250
    # the predicate is unchanged; the engine decides when to condemn
    assert a.violated_by(ZeusEvent("tool_called", "rm_rf"),
                         {"tool_declared": set()}) is True


def test_immediate_assert_has_no_grace():
    spec = parse("ASSERT no tool_called where identifier not in tool_declared\n")
    a = spec.assertions[0]
    assert a.grace_ms is None
    assert "grace_ms" not in a.detail
    assert a.violated_by(ZeusEvent("tool_called", "search"), {}) is True


@pytest.mark.parametrize("suffix", [
    " GRACE 0", " GRACE -1", " GRACE nope", " GRACE nan", " GRACE inf",
    " GRACE 250 EXTRA",
])
def test_grace_rejects_non_positive_or_malformed_values(suffix):
    with pytest.raises(ParseError) as exc:
        parse(
            "ASSERT no tool_called where identifier not in tool_declared"
            + suffix + "\n"
        )
    assert "line 1" in str(exc.value)


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


def _spec_file(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


def test_include_composes_fragment_rules_with_their_own_lines(tmp_path):
    from mo.parser import parse_file

    frag = _spec_file(
        tmp_path / "fragments" / "liveness.zspec",
        "; shared liveness rules\n"
        "on tool_declared as $tool:\n"
        "    EXPECT tool_called for $tool\n"
        "    WINDOW 5000\n",
    )
    root = _spec_file(
        tmp_path / "server.zspec",
        "INCLUDE fragments/liveness.zspec\n"
        "\n"
        "ASSERT no tool_called where identifier not in tool_declared\n",
    )

    spec = parse_file(str(root))

    assert len(spec.triggers) == 1 and len(spec.assertions) == 1
    trigger = spec.triggers[0]
    assert trigger.spec_line == 2                 # line inside the fragment
    assert trigger.spec_source == str(frag)
    assert trigger.spec_source.endswith("liveness.zspec")
    assert spec.assertions[0].spec_source == str(root)


def test_include_resolves_relative_to_the_including_fragment(tmp_path):
    from mo.parser import parse_file

    _spec_file(
        tmp_path / "shared" / "deep.zspec",
        "on tool_declared as $t:\n"
        "    EXPECT tool_called for $t\n"
        "    WINDOW 100\n",
    )
    _spec_file(tmp_path / "shared" / "mid.zspec", "INCLUDE deep.zspec\n")
    root = _spec_file(tmp_path / "root.zspec", "INCLUDE shared/mid.zspec\n")

    spec = parse_file(str(root))

    assert len(spec.triggers) == 1
    assert spec.triggers[0].window_ms == 100
    assert spec.triggers[0].spec_source.endswith("deep.zspec")


def test_diamond_include_composes_the_shared_fragment_once(tmp_path):
    from mo.parser import parse_file

    _spec_file(
        tmp_path / "common.zspec",
        "on tool_called as $t:\n"
        "    EXPECT tool_result for $t\n"
        "    WINDOW 250\n",
    )
    _spec_file(tmp_path / "a.zspec", "INCLUDE common.zspec\n")
    _spec_file(tmp_path / "b.zspec", "INCLUDE common.zspec\n")
    root = _spec_file(tmp_path / "root.zspec",
                      "INCLUDE a.zspec\nINCLUDE b.zspec\n")

    spec = parse_file(str(root))

    # one obligation per call, not two: the shared fragment was included once
    assert len(spec.triggers) == 1


def test_include_cycle_is_a_line_numbered_parse_error(tmp_path):
    from mo.parser import parse_file

    a = _spec_file(tmp_path / "a.zspec", "INCLUDE b.zspec\n")
    _spec_file(tmp_path / "b.zspec", "INCLUDE a.zspec\n")

    with pytest.raises(ParseError) as exc:
        parse_file(str(a))

    message = str(exc.value)
    assert "include cycle" in message
    assert "b.zspec: line 1" in message
    assert "a.zspec -> b.zspec -> a.zspec" in message


def test_missing_include_names_the_including_file_and_line(tmp_path):
    from mo.parser import parse_file

    root = _spec_file(tmp_path / "root.zspec", "INCLUDE nope.zspec\n")

    with pytest.raises(ParseError) as exc:
        parse_file(str(root))

    message = str(exc.value)
    assert "root.zspec: line 1" in message
    assert "cannot include 'nope.zspec'" in message


def test_error_inside_a_fragment_names_the_fragment(tmp_path):
    from mo.parser import parse_file

    _spec_file(tmp_path / "bad.zspec", "FROBNICATE everything\n")
    root = _spec_file(tmp_path / "root.zspec", "INCLUDE bad.zspec\n")

    with pytest.raises(ParseError) as exc:
        parse_file(str(root))

    message = str(exc.value)
    assert "bad.zspec: line 1" in message
    assert "unknown directive 'FROBNICATE'" in message
