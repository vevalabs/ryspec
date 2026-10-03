"""Check one ryspec document against the schema and the rules it cannot
check, by way of libryspec."""

from __future__ import annotations

from collections.abc import Iterable
from pathlib import Path

from ryspec import _core
from ryspec.diagnostic import Diagnostic


def lint_rules() -> tuple[int, ...]:
    """What libryspec checks, in order: 0, the schema, then the rules of
    SPEC.md's "What the schema cannot check" by number."""
    return _core.lint_rules()


def lint_file(path: Path, rules: Iterable[int] | None = None) -> list[Diagnostic]:
    """libryspec's one finding on the document at `path`: the reason it could
    not be parsed -- its TOML, or the grammar of an expression -- or the first
    violation of the checks, the schema's and then each rule's, or of `rules`
    alone when given. Empty when the document passes."""
    selected = None if rules is None else list(rules)
    return [
        Diagnostic(kind, f"{line}:{column}" if line else "", message)
        for kind, line, column, message in _core.lint(path, selected)
    ]
