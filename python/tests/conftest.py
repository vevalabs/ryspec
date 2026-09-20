from __future__ import annotations

import json
import subprocess
import tomllib
from pathlib import Path

import pytest
from jsonschema.validators import validator_for

REPO_ROOT = Path(__file__).resolve().parents[2]
SCHEMA_PATH = REPO_ROOT / "schemas" / "v0" / "ryspec.schema.json"
TOML_TEST_DIR = Path(__file__).resolve().parent / "vendor" / "toml-test" / "tests"


@pytest.fixture(scope="session")
def validator():
    schema = json.loads(SCHEMA_PATH.read_text())
    validator_cls = validator_for(schema)
    validator_cls.check_schema(schema)
    return validator_cls(schema)


def load_toml(path: Path) -> dict:
    return tomllib.loads(path.read_text())


@pytest.fixture(scope="session", autouse=True)
def ryspec_library():
    """Make sure libryspec is there before anything asks the loader a question.

    `ryspec.semantics` finds the library and never builds it -- a package
    should not shell out to cmake. The test suite may, and does, because
    running pytest in a fresh checkout should work without a separate build
    step. A wheel-installed package carries its own copy and this is a no-op.
    """
    from ryspec.semantics import LibraryNotFound, library_path

    try:
        return library_path()
    except LibraryNotFound:
        pass

    from tree_sitter_parser import build

    try:
        build()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        pytest.skip(f"cannot build libryspec: {error}")
    try:
        return library_path()
    except LibraryNotFound as error:
        pytest.skip(str(error))


@pytest.fixture(scope="session")
def ryspec_parser():
    """A tree_sitter.Parser holding the compiled ryspec grammar."""
    tree_sitter = pytest.importorskip("tree_sitter")
    from tree_sitter_parser import load_language

    try:
        language = load_language()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        pytest.skip(f"cannot build the ryspec parser: {error}")
    return tree_sitter.Parser(language)


@pytest.fixture(scope="session")
def toml_test_dir():
    """The standard TOML test suite, fetched by scripts/fetch-toml-test.sh."""
    if not TOML_TEST_DIR.is_dir():
        pytest.skip("toml-test is not present; run scripts/fetch-toml-test.sh")
    return TOML_TEST_DIR
