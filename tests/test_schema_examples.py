from __future__ import annotations

import tomllib
from pathlib import Path

import pytest

from ryspec.semantics import first_semantic_error

EXAMPLES_DIR = Path(__file__).resolve().parent.parent / "examples"


@pytest.mark.parametrize("path", sorted(EXAMPLES_DIR.glob("*.toml")), ids=lambda p: p.name)
def test_example_is_schema_and_semantically_valid(validator, path):
    document = tomllib.loads(path.read_text())

    schema_errors = list(validator.iter_errors(document))
    assert schema_errors == [], f"{path.name} failed schema validation: {schema_errors}"

    assert first_semantic_error(document) is None, f"{path.name} failed semantic validation"
