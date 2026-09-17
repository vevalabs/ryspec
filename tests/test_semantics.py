from __future__ import annotations

import tomllib
from pathlib import Path

import pytest

from ryspec.semantics import first_semantic_error

FIXTURES_DIR = Path(__file__).parent / "fixtures" / "invalid"


def load_toml(path: Path) -> dict:
    return tomllib.loads(path.read_text())

# fixture filename -> a substring expected in the reported pointer or message,
# specific enough to prove the right rule fired.
EXPECTED = {
    "monitor_list_overlap.toml": "listed in both",
    "duplicate_property_name.toml": "duplicate property name",
    "undeclared_monitor_entry.toml": "not declared in [variables]",
    "output_with_source.toml": "may not declare",
    "initial_value_without_partition.toml": "listed in no [monitor] list",
    "parameter_missing_initial_value.toml": "has no initial_value",
    "min_greater_than_max.toml": "is greater than max",
    "source_undeclared_head.toml": "undeclared variable",
    "rule_names_text_variable.toml": "no comparable value",
    "expression_without_flag.toml": "allow_expressions",
    "duplicate_source_of_value.toml": "more than one source of value",
}


@pytest.mark.parametrize("filename", sorted(EXPECTED))
def test_fixture_is_schema_valid_but_semantically_invalid(validator, filename):
    document = load_toml(FIXTURES_DIR / filename)

    schema_errors = list(validator.iter_errors(document))
    assert schema_errors == [], f"{filename} should be schema-valid, got {schema_errors}"

    error = first_semantic_error(document)
    assert error is not None, f"{filename} should trip a semantic check"
    assert EXPECTED[filename] in error, f"{filename}: unexpected error message: {error}"


def test_all_fixtures_covered():
    on_disk = {path.name for path in FIXTURES_DIR.glob("*.toml")}
    assert on_disk == set(EXPECTED)
