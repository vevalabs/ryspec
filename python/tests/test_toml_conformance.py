"""Run the standard TOML test suite through the ryspec grammar.

A ryspec document's `[extras]` table takes arbitrary keys, so whatever TOML can
say has to be sayable there. Each file in the suite is re-rooted under `extras`
and parsed: every valid one must parse clean, and every invalid one must be
rejected unless toml-test-semantic.txt beside this file says why it cannot be.
"""

from __future__ import annotations

import math
import tomllib
from pathlib import Path

from toml_reroot import reroot
from tree_sitter_parser import parse_error

SEMANTIC_EXCEPTIONS = Path(__file__).resolve().parent / "toml-test-semantic.txt"
# Collection runs before any fixture, so the suite is located from this file
# rather than from pytest's rootpath, which the C project owns.
SUITE_DIR = Path(__file__).resolve().parent / "vendor" / "toml-test" / "tests"
TOML_VERSION = "1.0.0"


def read_exceptions() -> set[str]:
    lines = SEMANTIC_EXCEPTIONS.read_text().splitlines()
    return {line for line in lines if line and not line.startswith("#")}


def suite_files(directory: Path, kind: str) -> list[str]:
    listing = directory / f"files-toml-{TOML_VERSION}"
    names = listing.read_text().split()
    return [n for n in names if n.startswith(f"{kind}/") and n.endswith(".toml")]


def as_ryspec(source: bytes) -> bytes:
    """The smallest ryspec document carrying `source` as its extras."""
    return b"version = 0\n[extras]\n" + reroot(source)


def pytest_generate_tests(metafunc):
    """Name every suite file as its own test, so a failure names the file."""
    directory = SUITE_DIR
    for kind in ("valid", "invalid"):
        fixture = f"{kind}_file"
        if fixture not in metafunc.fixturenames:
            continue
        names = suite_files(directory, kind) if directory.is_dir() else []
        metafunc.parametrize(fixture, names, ids=lambda n: n[len(kind) + 1 : -5])


def test_valid_document_parses(ryspec_parser, toml_test_dir, valid_file):
    source = (toml_test_dir / valid_file).read_bytes()
    error = parse_error(ryspec_parser, as_ryspec(source))
    assert error is None, f"{valid_file}: {error}"


def test_invalid_document_is_rejected(ryspec_parser, toml_test_dir, invalid_file):
    source = (toml_test_dir / invalid_file).read_bytes()
    error = parse_error(ryspec_parser, as_ryspec(source))
    if invalid_file in read_exceptions():
        assert error is None, (
            f"{invalid_file} is listed in {SEMANTIC_EXCEPTIONS.name} as beyond the"
            f" grammar, but the grammar now rejects it ({error}); drop the entry"
        )
    else:
        assert error is not None, f"{invalid_file} was accepted but is invalid TOML"


def test_every_exception_is_in_the_suite(toml_test_dir):
    listed = set(suite_files(toml_test_dir, "invalid"))
    stale = read_exceptions() - listed
    assert stale == set(), f"{SEMANTIC_EXCEPTIONS.name} names files the suite does not have"


def equivalent(left, right) -> bool:
    """Compare two decoded documents, counting NaN as equal to itself."""
    if isinstance(left, float) and isinstance(right, float):
        return left == right or (math.isnan(left) and math.isnan(right))
    if type(left) is not type(right):
        return False
    if isinstance(left, dict):
        return left.keys() == right.keys() and all(equivalent(left[k], right[k]) for k in left)
    if isinstance(left, list):
        return len(left) == len(right) and all(equivalent(a, b) for a, b in zip(left, right))
    return left == right


def test_rerooting_preserves_the_document(toml_test_dir, valid_file):
    """The conformance run is only worth as much as the transform it rests on."""
    source = (toml_test_dir / valid_file).read_bytes()
    rerooted = tomllib.loads(as_ryspec(source).decode())
    assert equivalent(rerooted["extras"], tomllib.loads(source.decode()))
