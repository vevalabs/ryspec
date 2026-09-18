"""Extra validation over a schema-valid ryspec document.

JSON Schema validates shape, not resolution. `first_semantic_error` covers what
README.md's "What the schema cannot check" section lists: cross-references
between `[variables]`, `[monitor]`'s three partition lists, rule/property/rule
names sharing one namespace, and a handful of other checks that turn on how
names resolve rather than how a single value is shaped.

Every function here assumes `document` already passed JSON Schema validation --
it never re-checks types or shapes, only cross-references. Calling it on a
schema-invalid document is undefined behavior.

Deduction (inferring the value space when `[monitor]`/`[variables]` are absent)
is out of scope: the checks below only examine `[monitor]` lists that are
explicitly present. An absent list contributes nothing, exactly because an
absent list and an empty one differ (see README.md's "Deduction" section) and
guessing what an absent list would deduce to is a distinct, larger feature.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Iterator

Pointer = tuple[object, ...]

COMPARISON_OPERATORS = {"lt", "le", "gt", "ge", "eq", "ne"}
UNARY_OPERATORS = {"not", "prev", "next"}
UNARY_TEMPORAL_OPERATORS = {"once", "historically", "eventually", "always"}
BINARY_TEMPORAL_OPERATORS = {"since", "until"}
MULTIARY_OPERATORS = {"and", "or", "equiv", "implies"}


def _pointer_str(pointer: Pointer) -> str:
    return "/" + "/".join(str(part) for part in pointer)


@dataclass(frozen=True)
class SemanticError:
    pointer: Pointer
    message: str

    def format(self) -> str:
        return f"{_pointer_str(self.pointer)}: {self.message}"


@dataclass(frozen=True)
class IdentifierUse:
    """A bare name read out of a rule -- a reference to whatever declares it."""

    name: str
    pointer: Pointer
    position: str  # "rule" | "predicate_subject" | "predicate_operand" | "bound"
    scope: object  # "file" | ("property", index)


@dataclass(frozen=True)
class ExpressionUse:
    text: str
    pointer: Pointer


@dataclass(frozen=True)
class BoundUse:
    bound: dict
    pointer: Pointer


def _is_expression(value: str) -> bool:
    return value.startswith("(") and value.endswith(")")


def _walk_rule(node: object, pointer: Pointer, scope: object) -> Iterator[object]:
    """Yield every IdentifierUse/ExpressionUse/BoundUse reachable from `node`.

    Dispatches on the operator string, which the schema already guarantees
    picks out exactly one of the seven rule shapes.
    """
    if isinstance(node, str):
        if _is_expression(node):
            yield ExpressionUse(node, pointer)
        else:
            yield IdentifierUse(node, pointer, "rule", scope)
        return

    operator = node[0]

    if operator in COMPARISON_OPERATORS:
        yield IdentifierUse(node[1], pointer + (1,), "predicate_subject", scope)
        operand = node[2]
        if isinstance(operand, str):
            yield IdentifierUse(operand, pointer + (2,), "predicate_operand", scope)
        return

    if operator in UNARY_OPERATORS:
        yield from _walk_rule(node[1], pointer + (1,), scope)
        return

    if operator in MULTIARY_OPERATORS:
        for index, sub_rule in enumerate(node[1:], start=1):
            yield from _walk_rule(sub_rule, pointer + (index,), scope)
        return

    if operator in UNARY_TEMPORAL_OPERATORS:
        yield from _walk_rule(node[1], pointer + (1,), scope)
        if len(node) == 3:
            yield from _walk_bound(node[2], pointer + (2,), scope)
        return

    if operator in BINARY_TEMPORAL_OPERATORS:
        yield from _walk_rule(node[1], pointer + (1,), scope)
        yield from _walk_rule(node[2], pointer + (2,), scope)
        if len(node) == 4:
            yield from _walk_bound(node[3], pointer + (3,), scope)
        return


def _walk_bound(bound: dict, pointer: Pointer, scope: object) -> Iterator[object]:
    yield BoundUse(bound, pointer)
    for key in ("min", "max"):
        value = bound.get(key)
        if isinstance(value, str):
            yield IdentifierUse(value, pointer + (key,), "bound", scope)


def _all_rule_positions(document: dict) -> Iterator[tuple[object, Pointer, object]]:
    for index, prop in enumerate(document.get("properties", [])):
        scope = ("property", index)
        if "given" in prop:
            yield prop["given"], ("properties", index, "given"), scope
        yield prop["check"], ("properties", index, "check"), scope
        for name, rule in prop.get("rules", {}).items():
            yield rule, ("properties", index, "rules", name), scope
    for name, rule in document.get("rules", {}).items():
        yield rule, ("rules", name), "file"


@dataclass
class Context:
    document: dict
    variables: dict[str, dict] = field(default_factory=dict)
    monitor: dict[str, list[str] | None] = field(default_factory=dict)
    file_rules: dict[str, Pointer] = field(default_factory=dict)
    property_names: list[tuple[str, Pointer]] = field(default_factory=list)
    property_private_rules: dict[int, dict[str, Pointer]] = field(default_factory=dict)


def _build_context(document: dict) -> Context:
    monitor = document.get("monitor") or {}
    properties = document.get("properties", [])
    return Context(
        document=document,
        variables=document.get("variables", {}) or {},
        monitor={
            "inputs": monitor.get("inputs"),
            "outputs": monitor.get("outputs"),
            "parameters": monitor.get("parameters"),
        },
        file_rules={name: ("rules", name) for name in document.get("rules", {})},
        property_names=[(prop["name"], ("properties", i, "name")) for i, prop in enumerate(properties)],
        property_private_rules={
            i: {name: ("properties", i, "rules", name) for name in prop.get("rules", {})}
            for i, prop in enumerate(properties)
        },
    )


def _check_monitor_overlap(ctx: Context) -> list[SemanticError]:
    errors = []
    present = [(name, names) for name, names in ctx.monitor.items() if names is not None]
    for i, (name_a, list_a) in enumerate(present):
        for name_b, list_b in present[i + 1 :]:
            for name in set(list_a) & set(list_b):
                index = list_b.index(name)
                errors.append(
                    SemanticError(
                        ("monitor", name_b, index),
                        f"'{name}' is listed in both '{name_a}' and '{name_b}'",
                    )
                )
    return errors


def _check_duplicate_property_names(ctx: Context) -> list[SemanticError]:
    errors = []
    seen: dict[str, Pointer] = {}
    for name, pointer in ctx.property_names:
        if name in seen:
            errors.append(
                SemanticError(
                    pointer,
                    f"duplicate property name '{name}' (also declared at {_pointer_str(seen[name])})",
                )
            )
        else:
            seen[name] = pointer
    return errors


def _unresolved_output(ctx: Context, name: str, pointer: Pointer) -> SemanticError | None:
    """Whether `name` in `outputs` reaches something that gives it a value.

    An output publishes a value that already exists somewhere in the file, so
    a `[variables]` entry is one way to name it and not the only one: a
    property publishes its verdict under its own name, a file-level rule
    publishes the rule's value, and a rule private to a property is reachable
    as `<property>.<rule>` -- the qualified form being the one way past the
    visibility boundary that `[properties.rules]` draws, without moving the
    rule to file level. A declaration remains the only way to give an output
    a `type`, a `unit` or a `description`.

    The schema types an output as a dotted path, which admits paths deeper
    than the one dot this form uses, so the arity is checked here.
    """
    if "." in name:
        segments = name.split(".")
        if len(segments) > 2:
            return SemanticError(
                pointer,
                f"'{name}' is listed in outputs but a qualified output names one property and one of its rules",
            )
        property_name, rule_name = segments
        index = next(
            (i for i, (declared, _) in enumerate(ctx.property_names) if declared == property_name),
            None,
        )
        if index is None:
            return SemanticError(pointer, f"'{name}' is listed in outputs but no property is named '{property_name}'")
        if rule_name not in ctx.property_private_rules.get(index, {}):
            return SemanticError(
                pointer,
                f"'{name}' is listed in outputs but property '{property_name}' declares no rule '{rule_name}'",
            )
        return None

    if name in ctx.variables or name in ctx.file_rules:
        return None
    if any(declared == name for declared, _ in ctx.property_names):
        return None
    return SemanticError(
        pointer,
        f"'{name}' is listed in outputs but names no variable, property or file-level rule",
    )


def _check_monitor_entries_declared(ctx: Context) -> list[SemanticError]:
    errors = []
    for list_name, names in ctx.monitor.items():
        if names is None:
            continue
        for index, name in enumerate(names):
            pointer = ("monitor", list_name, index)
            if list_name == "outputs":
                error = _unresolved_output(ctx, name, pointer)
                if error is not None:
                    errors.append(error)
            elif name not in ctx.variables:
                errors.append(
                    SemanticError(
                        pointer,
                        f"'{name}' is listed in {list_name} but not declared in [variables]",
                    )
                )
    return errors


_PARTITION_NOUN = {"outputs": "an output", "parameters": "a parameter"}


def _check_output_parameter_source_format(ctx: Context) -> list[SemanticError]:
    errors = []
    for list_name in ("outputs", "parameters"):
        names = ctx.monitor.get(list_name)
        if not names:
            continue
        for name in names:
            variable = ctx.variables.get(name)
            if variable is None:
                continue  # no [variables] declaration at all: rule 3's problem
            for key in ("source", "format"):
                if key in variable:
                    errors.append(
                        SemanticError(
                            ("variables", name, key),
                            f"'{name}' is {_PARTITION_NOUN[list_name]} and may not declare {key}",
                        )
                    )
    return errors


def _check_initial_value_without_partition(ctx: Context) -> list[SemanticError]:
    if ctx.monitor.get("parameters") is None:
        return []
    listed: set[str] = set()
    for list_name in ("inputs", "outputs", "parameters"):
        names = ctx.monitor.get(list_name)
        if names:
            listed.update(names)
    errors = []
    for name, variable in ctx.variables.items():
        if "initial_value" in variable and name not in listed:
            errors.append(
                SemanticError(
                    ("variables", name, "initial_value"),
                    f"'{name}' has an initial_value but is listed in no [monitor] list",
                )
            )
    return errors


def _check_parameter_missing_initial_value(ctx: Context) -> list[SemanticError]:
    names = ctx.monitor.get("parameters")
    if not names:
        return []
    errors = []
    for index, name in enumerate(names):
        variable = ctx.variables.get(name)
        if variable is None:
            continue  # no declaration at all: rule 3's problem
        if "initial_value" not in variable:
            errors.append(
                SemanticError(
                    ("monitor", "parameters", index),
                    f"parameter '{name}' has no initial_value",
                )
            )
    return errors


def _check_min_max(ctx: Context) -> list[SemanticError]:
    errors = []
    for name, variable in ctx.variables.items():
        low, high = variable.get("min"), variable.get("max")
        if low is not None and high is not None and low > high:
            errors.append(
                SemanticError(("variables", name, "min"), f"min ({low}) is greater than max ({high})")
            )
    for rule, pointer, scope in _all_rule_positions(ctx.document):
        for event in _walk_rule(rule, pointer, scope):
            if not isinstance(event, BoundUse):
                continue
            low, high = event.bound.get("min"), event.bound.get("max")
            if isinstance(low, (int, float)) and isinstance(high, (int, float)) and low > high:
                errors.append(SemanticError(event.pointer, f"min ({low}) is greater than max ({high})"))
    return errors


def _check_source_paths(ctx: Context) -> list[SemanticError]:
    errors = []
    for name, variable in ctx.variables.items():
        source = variable.get("source")
        if source is None:
            continue
        segments = source.split(".")
        head = segments[0]
        head_variable = ctx.variables.get(head)
        if head_variable is None:
            errors.append(
                SemanticError(
                    ("variables", name, "source"),
                    f"source '{source}' refers to undeclared variable '{head}'",
                )
            )
        elif len(segments) > 1 and "format" not in head_variable:
            errors.append(
                SemanticError(
                    ("variables", name, "source"),
                    f"source '{source}' points to '{head}', which declares no format",
                )
            )
    return errors


def _check_rule_text_binary(ctx: Context) -> list[SemanticError]:
    errors = []
    for rule, pointer, scope in _all_rule_positions(ctx.document):
        for event in _walk_rule(rule, pointer, scope):
            if not isinstance(event, IdentifierUse):
                continue
            variable = ctx.variables.get(event.name)
            if variable is not None and variable.get("type") in ("text", "binary"):
                errors.append(
                    SemanticError(
                        event.pointer,
                        f"rule refers to '{event.name}', a {variable['type']} variable with no comparable value",
                    )
                )
    return errors


def _check_source_of_value_collisions(ctx: Context) -> list[SemanticError]:
    """Two rules, a rule and a property, or a rule and an input cannot share a name.

    A name in an explicit [monitor] list with a matching [variables] entry may
    coincide with exactly one rule or property of that name -- the sanctioned
    "publish this rule's value" pattern -- and nothing else. Private,
    per-property rule tables are a visibility boundary: two properties may
    each declare their own same-named private rule without colliding, so
    those entries are scoped to their own property and never compared across
    properties. An identifier that resolves to nothing declared is an
    implicit input, discovered by walking every rule.
    """

    def declared_file_scope() -> dict[str, list[tuple[str, Pointer]]]:
        entries: dict[str, list[tuple[str, Pointer]]] = {}
        for name, pointer in ctx.file_rules.items():
            entries.setdefault(name, []).append(("rule", pointer))
        for name, pointer in ctx.property_names:
            entries.setdefault(name, []).append(("property", pointer))
        for list_name in ("inputs", "outputs", "parameters"):
            names = ctx.monitor.get(list_name)
            if not names:
                continue
            for index, name in enumerate(names):
                if "." in name:
                    continue  # a qualified output reaches a private rule; it declares nothing
                if name not in entries or all(kind != "variable" for kind, _ in entries[name]):
                    entries.setdefault(name, []).append(("variable", ("monitor", list_name, index)))
        return entries

    def visible_names(scope_index: int | None) -> set[str]:
        names = set(ctx.file_rules) | {name for name, _ in ctx.property_names} | set(ctx.variables)
        if scope_index is not None:
            names |= set(ctx.property_private_rules.get(scope_index, {}))
        return names

    def implicit_inputs_in_scope(scope_key: object) -> dict[str, Pointer]:
        found: dict[str, Pointer] = {}
        scope_index = scope_key[1] if isinstance(scope_key, tuple) else None
        visible = visible_names(scope_index)
        for rule, pointer, scope in _all_rule_positions(ctx.document):
            if scope != scope_key:
                continue
            for event in _walk_rule(rule, pointer, scope):
                if isinstance(event, IdentifierUse) and event.position == "rule" and event.name not in visible:
                    found.setdefault(event.name, event.pointer)
        return found

    def is_sanctioned(entries: list[tuple[str, Pointer]]) -> bool:
        kinds = [kind for kind, _ in entries]
        return sorted(kinds) == ["rule", "variable"] or sorted(kinds) == ["property", "variable"]

    file_entries = declared_file_scope()
    errors: list[SemanticError] = []
    reported: set[str] = set()

    property_count = len(ctx.property_private_rules)
    scope_keys: list[object] = ["file"] + [("property", i) for i in range(property_count)]

    for scope_key in scope_keys:
        group: dict[str, list[tuple[str, Pointer]]] = {
            name: list(entries) for name, entries in file_entries.items()
        }
        if isinstance(scope_key, tuple):
            for name, pointer in ctx.property_private_rules.get(scope_key[1], {}).items():
                group.setdefault(name, []).append(("rule", pointer))
        for name, pointer in implicit_inputs_in_scope(scope_key).items():
            group.setdefault(name, []).append(("implicit_input", pointer))

        for name, entries in group.items():
            if name in reported or len(entries) < 2:
                continue
            if is_sanctioned(entries):
                continue
            reported.add(name)
            second_pointer = entries[1][1]
            kinds = ", ".join(kind for kind, _ in entries)
            errors.append(SemanticError(second_pointer, f"'{name}' names more than one source of value ({kinds})"))

    return errors


_CHECKS = (
    _check_monitor_overlap,
    _check_duplicate_property_names,
    _check_monitor_entries_declared,
    _check_output_parameter_source_format,
    _check_initial_value_without_partition,
    _check_parameter_missing_initial_value,
    _check_min_max,
    _check_source_paths,
    _check_rule_text_binary,
    _check_source_of_value_collisions,
)


def all_semantic_errors(document: dict) -> list[SemanticError]:
    ctx = _build_context(document)
    errors: list[SemanticError] = []
    for check in _CHECKS:
        errors.extend(check(ctx))
    return errors


def first_semantic_error(document: dict) -> str | None:
    """The best single semantic error for a schema-valid document, or None.

    Only meaningful once schema validation has already passed -- every check
    here assumes `document` matches the schema's shapes and only checks
    cross-references the schema cannot express.
    """
    errors = all_semantic_errors(document)
    if not errors:
        return None
    return errors[0].format()
