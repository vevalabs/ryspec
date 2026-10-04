"""Check one ryspec document against the schema and the rules it cannot
check, by way of libryspec."""

from __future__ import annotations

from pathlib import Path

from ryspec import _core
from ryspec.diagnostic import Diagnostic


def lint_file(path: Path) -> list[Diagnostic]:
    """libryspec's one finding on the document at `path`: the reason it could
    not be parsed -- its TOML, or the grammar of an expression -- or the first
    violation of the checks, the schema's and then each rule's. Empty when
    the document passes."""
    return [
        Diagnostic(kind, f"{line}:{column}" if line else "", message)
        for kind, line, column, message in _core.lint(path)
    ]
