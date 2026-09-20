from __future__ import annotations

import tomllib
from pathlib import Path

import pytest

from ryspec.semantics import first_semantic_error

EXAMPLES_DIR = Path(__file__).resolve().parents[2] / "examples"


@pytest.mark.parametrize("path", sorted(EXAMPLES_DIR.glob("*.toml")), ids=lambda p: p.name)
def test_example_is_schema_and_semantically_valid(validator, path):
    text = path.read_text()

    schema_errors = list(validator.iter_errors(tomllib.loads(text)))
    assert schema_errors == [], f"{path.name} failed schema validation: {schema_errors}"

    error = first_semantic_error(text, path)
    assert error is None, f"{path.name} failed semantic validation: {error}"
