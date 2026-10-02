"""Check one ryspec document against the rules the schema cannot check, by
way of libryspec."""

from __future__ import annotations

from pathlib import Path

from ryspec import _core
from ryspec.validate import Diagnostic


def lint_file(path: Path) -> list[Diagnostic]:
    """Every finding of libryspec's linter on the document at `path`, or the
    one reason it could not be parsed. Empty when the document passes."""
    return [
        Diagnostic(kind, f"{line}:{column}" if line else "", message)
        for kind, line, column, message in _core.lint(path)
    ]
