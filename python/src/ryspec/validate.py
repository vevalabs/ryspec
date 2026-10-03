"""Check one ryspec document: TOML syntax first, then the JSON Schema."""

from __future__ import annotations

import json
from functools import lru_cache
from importlib import resources
from pathlib import Path

import jsonschema
import tomli

from ryspec.diagnostic import Diagnostic

SCHEMA_DIRECTIVE = "#:schema"

# The schema a wheel ships, and where it sits in a source checkout.
_PACKAGED_SCHEMA = ("schemas", "v0", "ryspec.schema.json")
_CHECKOUT_SCHEMA = Path(__file__).resolve().parents[3] / "schemas" / "v0" / "ryspec.schema.json"


def schema_directive(text: str) -> str | None:
    """The `#:schema` header's target, read from the comments that open the file."""
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        if not stripped.startswith("#"):
            return None
        if stripped.startswith(SCHEMA_DIRECTIVE + " "):
            return stripped[len(SCHEMA_DIRECTIVE) :].strip()
    return None


def default_schema_path() -> Path:
    packaged = resources.files("ryspec").joinpath(*_PACKAGED_SCHEMA)
    if packaged.is_file():
        return Path(str(packaged))
    return _CHECKOUT_SCHEMA


@lru_cache(maxsize=None)
def load_validator(schema_path: Path) -> jsonschema.protocols.Validator:
    schema = json.loads(schema_path.read_text(encoding="utf-8"))
    cls = jsonschema.validators.validator_for(schema)
    cls.check_schema(schema)
    return cls(schema, format_checker=cls.FORMAT_CHECKER)


def resolve_schema(path: Path, text: str, override: Path | None) -> Path:
    """`override` wins, then the file's own `#:schema` header when it names a
    local file, then the schema this package ships."""
    if override is not None:
        return override
    target = schema_directive(text)
    if target and "://" not in target:
        return (path.parent / target).resolve()
    return default_schema_path()


def validate_file(path: Path, schema: Path | None = None) -> list[Diagnostic]:
    """Every diagnostic for the document at `path`; empty when it is valid."""
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        return [Diagnostic("io", "", str(exc))]

    try:
        document = tomli.loads(text)
    except tomli.TOMLDecodeError as exc:
        return [Diagnostic("toml", f"{exc.lineno}:{exc.colno}", exc.msg)]

    schema_path = resolve_schema(path, text, schema)
    try:
        validator = load_validator(schema_path)
    except (OSError, ValueError, jsonschema.SchemaError) as exc:
        return [Diagnostic("io", "", f"cannot load schema {schema_path}: {exc}")]

    errors = sorted(validator.iter_errors(document), key=lambda e: list(map(str, e.absolute_path)))
    return [Diagnostic("schema", e.json_path, e.message) for e in errors]
