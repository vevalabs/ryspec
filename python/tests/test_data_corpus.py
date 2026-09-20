"""The corpus under data/ -- documents the format accepts, and documents it rejects.

Where fixtures/invalid holds one fixture per rule in README.md's "What
the schema cannot check", this corpus is the broader one: data/valid covers
each construct the format offers, and data/invalid covers rejection at both
layers -- the schema's, which fixtures/ deliberately never exercises
because every fixture there is schema-valid by construction, and the loader's.

A negative file declares its own expectation in a header comment, so extending
the corpus means adding a file and nothing else:

    #:expect-schema-error <substring of the reported error>
    #:expect-semantic-error <substring of the reported error>
    #:expect-grammar-error

The marker names the layer as well as the message, which is the part worth
pinning: a file meant to fail the schema that instead slips through to a
semantic error is testing something other than what it claims to. The grammar
marker carries no message, because the parser's error strings are positions
rather than an API worth freezing.

data/malformed is the grammar's own half, and it exists because `ryspec
validate` cannot reject those files: the schema types an expression as any
parenthesised string and the loader never looks inside one, so a malformed
expression is well-formed to both. Keeping those documents out of
data/invalid is what lets `ryspec validate data/invalid --expect-invalid`
stay an honest assertion.
"""

from __future__ import annotations

import re
import tomllib
from pathlib import Path

import pytest
from tree_sitter_parser import parse_error

from ryspec import first_error
from ryspec.semantics import first_semantic_error

REPO_ROOT = Path(__file__).resolve().parents[2]
SCHEMA = REPO_ROOT / "schemas" / "v0" / "ryspec.schema.json"
VALID_DIR = REPO_ROOT / "data" / "valid"
INVALID_DIR = REPO_ROOT / "data" / "invalid"
MALFORMED_DIR = REPO_ROOT / "data" / "malformed"

EXPECTATION = re.compile(r"^#:expect-(schema|semantic|grammar)-error ?(.*)$", re.MULTILINE)

# rglob, because operators/ holds a document per temporal operator on each side.
VALID_FILES = sorted(VALID_DIR.rglob("*.toml"))
INVALID_FILES = sorted(INVALID_DIR.rglob("*.toml"))
MALFORMED_FILES = sorted(MALFORMED_DIR.rglob("*.toml"))


def expectation(path: Path) -> tuple[str, str]:
    """The (layer, substring) a negative file declares in its header."""
    matches = EXPECTATION.findall(path.read_text())
    assert len(matches) == 1, f"{path.name} declares {len(matches)} expectations, want exactly 1"
    layer, substring = matches[0]
    return layer, substring.strip()


def diagnose(validator, path: Path) -> tuple[str, str]:
    """The layer that rejects `path` and the error it reports, ('none', '') if neither.

    The two layers `ryspec validate` reaches, in its order: the schema's, and
    the loader's, which is the C library. The grammar's is a third and is not
    here, which is what data/malformed turns on.
    """
    text = path.read_text()
    schema_error = first_error(validator, tomllib.loads(text))
    if schema_error is not None:
        return "schema", schema_error
    semantic_error = first_semantic_error(text, path)
    if semantic_error is not None:
        return "semantic", semantic_error
    return "none", ""


def test_the_corpus_is_not_empty():
    assert VALID_FILES, f"no *.toml under {VALID_DIR}"
    assert INVALID_FILES, f"no *.toml under {INVALID_DIR}"
    assert MALFORMED_FILES, f"no *.toml under {MALFORMED_DIR}"


@pytest.mark.parametrize(
    "path", VALID_FILES + INVALID_FILES + MALFORMED_FILES, ids=lambda p: str(p.relative_to(REPO_ROOT / "data"))
)
def test_corpus_file_points_at_the_schema(path):
    """The #:schema header is what an editor reads, and its path is easy to get wrong."""
    first_line = path.read_text().splitlines()[0]
    assert first_line.startswith("#:schema "), f"{path.name} has no #:schema header"
    referenced = (path.parent / first_line.removeprefix("#:schema ").strip()).resolve()
    assert referenced == SCHEMA, f"{path.name} points at {referenced}, not the bundled schema"


@pytest.mark.parametrize("path", VALID_FILES, ids=lambda p: str(p.relative_to(REPO_ROOT / "data")))
def test_valid_corpus_file_is_accepted(validator, path):
    layer, error = diagnose(validator, path)
    assert layer == "none", f"{path.name} should validate, got a {layer} error: {error}"


@pytest.mark.parametrize("path", VALID_FILES, ids=lambda p: str(p.relative_to(REPO_ROOT / "data")))
def test_valid_corpus_file_parses(ryspec_parser, path):
    error = parse_error(ryspec_parser, path.read_bytes())
    assert error is None, f"{path.name}: {error}"


@pytest.mark.parametrize("path", INVALID_FILES, ids=lambda p: str(p.relative_to(REPO_ROOT / "data")))
def test_invalid_corpus_file_is_rejected_where_it_says(validator, path):
    want_layer, want_substring = expectation(path)
    assert want_layer != "grammar", f"{path.name} belongs under data/malformed, which ryspec validate cannot reject"
    assert want_substring, f"{path.name} declares no substring to match"
    layer, error = diagnose(validator, path)
    assert layer == want_layer, f"{path.name} expected a {want_layer} error, got {layer}: {error}"
    assert want_substring in error, f"{path.name}: {error!r} does not contain {want_substring!r}"


@pytest.mark.parametrize("path", INVALID_FILES, ids=lambda p: str(p.relative_to(REPO_ROOT / "data")))
def test_semantically_invalid_corpus_file_still_parses(ryspec_parser, path):
    """Semantic invalidity is beyond the grammar, so those files must still parse.

    A schema-invalid file carries no such obligation: a bad operator or a bad
    rule arity is exactly the kind of thing the grammar is entitled to reject.
    """
    if expectation(path)[0] != "semantic":
        pytest.skip("only a semantically invalid document has to parse")
    error = parse_error(ryspec_parser, path.read_bytes())
    assert error is None, f"{path.name} is invalid semantically, not syntactically: {error}"


@pytest.mark.parametrize("path", MALFORMED_FILES, ids=lambda p: str(p.relative_to(REPO_ROOT / "data")))
def test_malformed_corpus_file_is_well_formed_to_the_validator(validator, path):
    """The layer boundary, asserted from below.

    An expression is an opaque string to the schema and the loader alike, so
    these documents pass `ryspec validate`. If one ever stops passing, the
    fault it carries is no longer the grammar's alone and the file belongs
    under data/invalid instead.
    """
    assert expectation(path)[0] == "grammar", f"{path.name} is not a grammar fixture"
    layer, error = diagnose(validator, path)
    assert layer == "none", f"{path.name} should reach the parser untouched, got a {layer} error: {error}"


@pytest.mark.parametrize("path", MALFORMED_FILES, ids=lambda p: str(p.relative_to(REPO_ROOT / "data")))
def test_malformed_corpus_file_is_rejected_by_the_grammar(ryspec_parser, path):
    assert parse_error(ryspec_parser, path.read_bytes()) is not None, (
        f"{path.name} parses cleanly, so it documents no syntax error"
    )
