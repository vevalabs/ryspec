"""The ryspec tree-sitter grammar over ryspec documents.

The grammar is the finest syntactic description of the format, so a document
the loader rejects for a semantic reason still has to parse: the fixtures under
fixtures/invalid are syntax the grammar accepts and semantics it cannot
see. What the grammar owes is the other direction -- malformed syntax must not
parse quietly.
"""

from __future__ import annotations

from pathlib import Path

import pytest
from tree_sitter_parser import parse_error

REPO_ROOT = Path(__file__).resolve().parents[2]
EXAMPLES_DIR = REPO_ROOT / "examples"
INVALID_FIXTURES = Path(__file__).resolve().parent / "fixtures" / "invalid"

MALFORMED = {
    "two pairs on one line": 'version = 0\nnamespace = "a.b" title = "c"\n',
    "a pair on the header line": 'version = 0\n[[properties]] name = "p"\n',
    "an unterminated string": 'version = 0\n\n[meta]\ntitle = "unfinished\n',
    "a rule with no operator": 'version = 0\n\n[[properties]]\nname = "p"\ncheck = ["nope", "p"]\n',
    "an unbalanced expression": 'version = 0\n\n[[properties]]\nname = "p"\ncheck = "(once[3:10] {p}"\n',
    "an inline table across lines": 'version = 0\n\n[variables]\nspeed = {\n  type = "number" }\n',
    "a version that is not a number": 'version = "0"\n',
    "a bound outside a temporal rule": 'version = 0\n\n[[properties]]\nname = "p"\ncheck = ["not", "p", { max = 3 }]\n',
}


@pytest.mark.parametrize("path", sorted(EXAMPLES_DIR.glob("*.toml")), ids=lambda p: p.name)
def test_example_parses(ryspec_parser, path):
    error = parse_error(ryspec_parser, path.read_bytes())
    assert error is None, f"{path.name}: {error}"


@pytest.mark.parametrize("path", sorted(INVALID_FIXTURES.glob("*.toml")), ids=lambda p: p.name)
def test_semantically_invalid_fixture_still_parses(ryspec_parser, path):
    error = parse_error(ryspec_parser, path.read_bytes())
    assert error is None, f"{path.name} is invalid semantically, not syntactically: {error}"


@pytest.mark.parametrize("source", MALFORMED.values(), ids=list(MALFORMED))
def test_malformed_document_is_rejected(ryspec_parser, source):
    assert parse_error(ryspec_parser, source.encode()) is not None


def test_crlf_line_endings_parse(ryspec_parser):
    source = 'version = 0\r\n\r\n[[properties]]\r\nname = "p"\r\ncheck = "p"\r\n'
    assert parse_error(ryspec_parser, source.encode()) is None


def test_a_file_without_a_final_newline_parses(ryspec_parser):
    assert parse_error(ryspec_parser, b'version = 0\n\n[meta]\ntitle = "x"') is None
