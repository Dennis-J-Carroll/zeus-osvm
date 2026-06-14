"""The .zspec text parser (M1). Line-oriented, like the zasm assembler.

Produces the same rules.Spec / TriggerRule / AssertRule objects the engine
already consumes, so M0's loop is untouched. Grammar v0:

    ; comment to end of line
    on <primitive> as $<bind>:
        EXPECT <primitive> for $<bind>
        WINDOW <ms>

    ASSERT no <primitiveA> where identifier not in <primitiveB>

Errors carry a 1-based line number (assembler-style) so a bad spec points at
the offending line rather than failing opaquely.
"""
from __future__ import annotations

from .events import ZeusEvent
from .rules import AssertRule, Spec, TriggerRule


class ParseError(Exception):
    pass


def _membership_predicate(primitive_a: str, primitive_b: str):
    # violation: an event of primitive_a whose identifier was never seen on
    # any primitive_b event. (factory avoids loop-closure capture bugs)
    def pred(ev: ZeusEvent, seen: dict) -> bool:
        return (ev.primitive == primitive_a
                and ev.identifier not in seen.get(primitive_b, set()))
    return pred


def _strip_comment(raw: str) -> str:
    return raw.split(";", 1)[0]


class _Block:
    def __init__(self, on_primitive: str, bind: str, line: int):
        self.on_primitive = on_primitive
        self.bind = bind
        self.line = line
        self.expects: str | None = None
        self.window: float | None = None


def _parse_on(tokens: list[str], line: int) -> _Block:
    # on <primitive> as $<bind>:
    if len(tokens) != 4 or tokens[2] != "as" or not tokens[3].startswith("$"):
        raise ParseError(f"line {line}: expected 'on <primitive> as $<bind>:'")
    return _Block(on_primitive=tokens[1], bind=tokens[3][1:], line=line)


def _parse_expect(tokens: list[str], block: _Block, line: int) -> None:
    # EXPECT <primitive> for $<bind>
    if len(tokens) != 4 or tokens[2] != "for" or not tokens[3].startswith("$"):
        raise ParseError(f"line {line}: expected 'EXPECT <primitive> for $<bind>'")
    bind = tokens[3][1:]
    if bind != block.bind:
        raise ParseError(
            f"line {line}: binding ${bind} does not match block binding ${block.bind}"
        )
    block.expects = tokens[1]


def _parse_window(tokens: list[str], block: _Block, line: int) -> None:
    # WINDOW <ms>
    if len(tokens) != 2:
        raise ParseError(f"line {line}: expected 'WINDOW <ms>'")
    try:
        block.window = float(tokens[1])
    except ValueError:
        raise ParseError(f"line {line}: WINDOW value '{tokens[1]}' is not a number")


def _parse_assert(tokens: list[str], line: int) -> AssertRule:
    # ASSERT no <A> where identifier not in <B>
    shape = ["ASSERT", "no", None, "where", "identifier", "not", "in", None]
    if len(tokens) != 8 or any(
        want is not None and got != want for got, want in zip(tokens, shape)
    ):
        raise ParseError(
            f"line {line}: expected "
            "'ASSERT no <primitive> where identifier not in <primitive>'"
        )
    primitive_a, primitive_b = tokens[2], tokens[7]
    return AssertRule(
        spec_line=line,
        predicate=_membership_predicate(primitive_a, primitive_b),
        detail={"rule": f"no {primitive_a} where identifier not in {primitive_b}"},
    )


def _finalize(block: _Block) -> TriggerRule:
    if block.expects is None:
        raise ParseError(f"line {block.line}: 'on' block missing EXPECT")
    if block.window is None:
        raise ParseError(f"line {block.line}: 'on' block missing WINDOW")
    return TriggerRule(
        on_primitive=block.on_primitive,
        expects=block.expects,
        window_ms=block.window,
        spec_line=block.line,
    )


def parse(text: str) -> Spec:
    spec = Spec()
    block: _Block | None = None

    for n, raw in enumerate(text.splitlines(), start=1):
        content = _strip_comment(raw)
        if not content.strip():
            continue
        indented = content[:1].isspace()
        tokens = content.replace(":", " ").split()

        if indented:
            if block is None:
                raise ParseError(f"line {n}: indented line outside any 'on' block")
            if tokens[0] == "EXPECT":
                _parse_expect(tokens, block, n)
            elif tokens[0] == "WINDOW":
                _parse_window(tokens, block, n)
            else:
                raise ParseError(f"line {n}: unknown directive '{tokens[0]}'")
            continue

        # top-level line: finalize any open block first
        if block is not None:
            spec.triggers.append(_finalize(block))
            block = None

        if tokens[0] == "on":
            block = _parse_on(tokens, n)
        elif tokens[0] == "ASSERT":
            spec.assertions.append(_parse_assert(tokens, n))
        else:
            raise ParseError(f"line {n}: unknown directive '{tokens[0]}'")

    if block is not None:
        spec.triggers.append(_finalize(block))

    return spec


def parse_file(path: str) -> Spec:
    with open(path, "r") as fh:
        return parse(fh.read())
