#!/usr/bin/env python3
"""ryspec command-line tool.

`ryspec validate` recursively validates every *.toml file under a directory
(default: the current directory) against schemas/v0/ryspec.schema.json. Pass
--expect-invalid to assert the opposite -- that every file in that directory
FAILS validation -- which is useful for checking a directory of negative test
fixtures.

Exits 0 when every file matches its expectation.
"""

from __future__ import annotations

import argparse
import json
import sys
import tomllib
from pathlib import Path

from jsonschema.exceptions import best_match
from jsonschema.protocols import Validator
from jsonschema.validators import validator_for

from ryspec.semantics import first_semantic_error

DEFAULT_SCHEMA = Path(__file__).resolve().parent / "schemas" / "v0" / "ryspec.schema.json"


def first_error(validator: Validator, document: object) -> str | None:
    """The best single error for a document, or None if it validates.

    best_match keeps a report to one line. It cannot do much for a rule: the
    seven branches of the rule `anyOf` are structurally alike, so a malformed
    rule reports the whole rule rather than the operator or the arity at fault.
    """
    error = best_match(validator.iter_errors(document))
    if error is None:
        return None
    pointer = "/" + "/".join(str(part) for part in error.absolute_path)
    return f"{pointer}: {error.message}"


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    subparsers = parser.add_subparsers(dest="command", required=True)

    validate = subparsers.add_parser("validate", help="validate *.toml specs against the schema")
    validate.add_argument(
        "directory",
        nargs="?",
        type=Path,
        default=Path("."),
        help="directory to search for *.toml specs, recursively (default: current directory)",
    )
    validate.add_argument(
        "--schema",
        type=Path,
        default=DEFAULT_SCHEMA,
        help=f"path to the JSON schema (default: {DEFAULT_SCHEMA})",
    )
    validate.add_argument(
        "--expect-invalid",
        action="store_true",
        help="assert every file FAILS validation, instead of passing (for negative fixtures)",
    )

    return parser.parse_args(argv)


def cmd_validate(args: argparse.Namespace) -> int:
    if not args.directory.is_dir():
        print(f"error: {args.directory} is not a directory", file=sys.stderr)
        return 1

    schema = json.loads(args.schema.read_text())
    validator_cls = validator_for(schema, default=None)
    if validator_cls is None:
        print(f"error: unrecognized $schema in {args.schema}: {schema.get('$schema')!r}", file=sys.stderr)
        return 1
    # A mis-typed keyword fails open -- `additionalItems` misspelt is simply
    # ignored -- so check the schema is a schema before trusting any verdict.
    validator_cls.check_schema(schema)
    validator = validator_cls(schema)

    files = sorted(args.directory.rglob("*.toml"))
    if not files:
        print(f"no .toml files found under {args.directory}", file=sys.stderr)
        return 1

    failures: list[str] = []
    for path in files:
        document = tomllib.loads(path.read_text())
        error = first_error(validator, document)
        if error is None:
            error = first_semantic_error(document)
        if args.expect_invalid:
            if error is None:
                failures.append(f"{path}: validated, but is a negative fixture")
        elif error is not None:
            failures.append(f"{path}: {error}")

    for failure in failures:
        print(f"FAIL {failure}", file=sys.stderr)

    if failures:
        print(f"\n{len(failures)} of {len(files)} file(s) failed", file=sys.stderr)
        return 1

    print(f"ok: {len(files)} file(s) validated against {args.schema}")
    return 0


COMMANDS = {"validate": cmd_validate}


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    return COMMANDS[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
