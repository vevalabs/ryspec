"""One fixture per rule in README.md's "What the schema cannot check".

The checks themselves are the C library's -- `ryspec.semantics` is a binding
over `src/semantics.c`, and holds none of them. What this file pins is the
contract between the two layers: every fixture here is schema-valid by
construction, so the diagnostic it trips can only be the loader's, and the
substring each one is held to is that diagnostic's wording.

`ryspec-db --expect-invalid python/tests/fixtures/invalid` is the same corpus
from the other side, under ctest as `db-fixtures`.
"""

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
    "unresolved_output.toml": "names no variable, property or file-level rule",
    "output_qualified_unknown_property.toml": "no property is named",
    "output_qualified_unknown_rule.toml": "declares no rule",
    "output_qualified_too_deep.toml": "one property and one of its rules",
    "output_with_source.toml": "may not declare",
    "initial_value_without_partition.toml": "listed in no [monitor] list",
    "parameter_missing_initial_value.toml": "has no initial_value",
    "min_greater_than_max.toml": "is greater than max",
    "source_undeclared_head.toml": "undeclared variable",
    "rule_names_text_variable.toml": "no comparable value",
    "duplicate_source_of_value.toml": "more than one source of value (rule, property)",
    # Rule 11 once per pair of sources that may share a name and does not.
    "rule_and_input_share_name.toml": "more than one source of value (rule, input)",
    "rule_and_parameter_share_name.toml": "more than one source of value (rule, parameter)",
    "property_and_input_share_name.toml": "more than one source of value (property, input)",
    "file_rule_and_private_rule_share_name.toml": "more than one source of value (rule, rule)",
    "property_and_private_rule_share_name.toml": "more than one source of value (property, rule)",
    # Rule 12 once per shape a mixed cone hides behind a name, and rule 13.
    "cone_mixed_through_rule_name.toml": "'always' looks to the future and 'looks_back' is a past-time rule",
    "cone_mixed_in_a_conjunction.toml": "'looks_ahead' is a future-time rule and the operand at",
    "cone_mixed_through_private_rule.toml": "'once' looks to the past and 'ahead' is a future-time rule",
    "cone_mixed_through_property_verdict.toml": "'always' looks to the future and 'backward' is a past-time rule",
    "rule_defined_in_terms_of_itself.toml": "is defined in terms of itself",
}

# Documents the checks accept, each one a shape a check could plausibly but
# wrongly reject: a name two declarations share legitimately, and a cone that
# resolves cleanly through the names a rule reads.
# Each is a whole document, because what makes it legal is how the parts sit
# together -- `outputs` binding a declaration to the rule that gives it a
# value, or a visibility boundary keeping two same-named rules apart.
ACCEPTED = {
    "an output binding a declaration to a file-level rule of that name": """
version = 0
[monitor]
inputs = ["p"]
outputs = ["guard"]
[variables]
p = { type = "bool" }
guard = { type = "bool", description = "the published guard" }
[rules]
guard = ["once", "p"]
[[properties]]
name = "guard_holds"
check = "guard"
""",
    "an output publishing a property verdict under the property's name": """
version = 0
[monitor]
inputs = ["p"]
outputs = ["guard_holds"]
[variables]
p = { type = "bool" }
guard_holds = { type = "bool" }
[[properties]]
name = "guard_holds"
check = ["once", "p"]
""",
    "a rule with no direction of its own, read from both cones": """
version = 0
[monitor]
inputs = ["p", "q"]
outputs = ["backward", "forward"]
[variables]
p = { type = "bool" }
q = { type = "bool" }
[rules]
both = ["and", "p", "q"]
[[properties]]
name = "backward"
check = ["once", "both"]
[[properties]]
name = "forward"
check = ["always", "both"]
""",
    "a past-time rule named inside another past-time rule": """
version = 0
[monitor]
inputs = ["p"]
outputs = ["fine"]
[variables]
p = { type = "bool" }
[rules]
looks_back = ["once", "p"]
[[properties]]
name = "fine"
check = ["historically", "looks_back"]
""",
    "two properties each declaring their own private rule of one name": """
version = 0
[monitor]
inputs = ["p", "q"]
outputs = ["left_holds", "right_holds"]
[variables]
p = { type = "bool" }
q = { type = "bool" }
[[properties]]
name = "left_holds"
check = ["and", "p", "rhs"]
[properties.rules]
rhs = ["once", "q"]
[[properties]]
name = "right_holds"
check = ["and", "q", "rhs"]
[properties.rules]
rhs = ["historically", "p"]
""",
}


@pytest.mark.parametrize("filename", sorted(EXPECTED))
def test_fixture_is_schema_valid_but_semantically_invalid(validator, filename):
    path = FIXTURES_DIR / filename
    text = path.read_text()

    schema_errors = list(validator.iter_errors(tomllib.loads(text)))
    assert schema_errors == [], f"{filename} should be schema-valid, got {schema_errors}"

    error = first_semantic_error(text, path)
    assert error is not None, f"{filename} should trip a semantic check"
    assert EXPECTED[filename] in error, f"{filename}: unexpected error message: {error}"


@pytest.mark.parametrize("description", sorted(ACCEPTED))
def test_a_shared_name_the_namespace_allows(validator, description):
    text = ACCEPTED[description]

    schema_errors = list(validator.iter_errors(tomllib.loads(text)))
    assert schema_errors == [], f"{description}: should be schema-valid, got {schema_errors}"

    error = first_semantic_error(text)
    assert error is None, f"{description}: should be accepted, got {error}"


# The one place the loader reaches further than it used to. This document was
# accepted while the checks were written in Python, and the reason was a limit
# rather than a rule: an expression was an opaque string to that loader, so the
# cones inside one went unread. The C loader parses both spellings into the
# same rules, so it resolves these two names and finds what the README forbids
# anywhere else -- a rule holding both cones at once.
#
# Nothing in data/ or examples/ turns on this; it is here so the change is
# recorded rather than discovered.
CONE_MIXED_INSIDE_AN_EXPRESSION = """
version = 0
[monitor]
inputs = ["p"]
outputs = ["opaque"]
[variables]
p = { type = "bool" }
[rules]
looks_back = ["once", "p"]
looks_ahead = ["eventually", "p"]
[[properties]]
name = "opaque"
check = "({looks_back} and {looks_ahead})"
"""


def test_a_cone_mixed_inside_an_expression_is_reported(validator):
    document = tomllib.loads(CONE_MIXED_INSIDE_AN_EXPRESSION)
    assert list(validator.iter_errors(document)) == [], "the schema sees an opaque string here"

    error = first_semantic_error(CONE_MIXED_INSIDE_AN_EXPRESSION)
    assert error is not None, "expression form is no longer opaque to the loader"
    assert "is a future-time rule" in error, error


def test_all_fixtures_covered():
    on_disk = {path.name for path in FIXTURES_DIR.glob("*.toml")}
    assert on_disk == set(EXPECTED)
