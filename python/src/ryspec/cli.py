"""The `ryspec` command line."""

from __future__ import annotations

import argparse
import sys
from collections.abc import Callable, Iterable, Iterator
from pathlib import Path

from ryspec.diagnostic import Diagnostic


def toml_files(paths: Iterable[Path]) -> Iterator[Path]:
    """Each path that is a file, and every `.toml` file under each directory,
    skipping hidden directories."""
    for path in paths:
        if path.is_dir():
            for found in sorted(path.rglob("*.toml")):
                relative = found.relative_to(path).parts[:-1]
                if found.is_file() and not any(part.startswith(".") for part in relative):
                    yield found
        else:
            yield path


def check_files(args: argparse.Namespace, check: Callable[[Path], list[Diagnostic]]) -> int:
    """Run `check` over every file under `args.paths`, and report each file's
    diagnostics, then a summary."""
    missing = [p for p in args.paths if not p.exists()]
    for path in missing:
        print(f"ryspec: {path}: no such file or directory", file=sys.stderr)
    if missing:
        return 2

    checked = failed = 0
    for path in toml_files(args.paths):
        checked += 1
        diagnostics = check(path)
        if diagnostics:
            failed += 1
            for diagnostic in diagnostics:
                print(f"{path}: {diagnostic}")
        elif not args.quiet:
            print(f"{path}: ok")

    sys.stdout.flush()
    print(f"{checked} checked, {checked - failed} valid, {failed} invalid", file=sys.stderr)
    return 1 if failed else 0


def run_validate(args: argparse.Namespace) -> int:
    from ryspec.validate import validate_file

    return check_files(args, lambda path: validate_file(path, args.schema))


def run_lint(args: argparse.Namespace) -> int:
    from ryspec.lint import lint_file, lint_rules

    if args.rules:
        unknown = sorted(set(args.rules) - set(lint_rules()))
        if unknown:
            checked = ", ".join(map(str, lint_rules()))
            print(f"ryspec: no lint rule {unknown[0]} (the rules checked: {checked})",
                  file=sys.stderr)
            return 2
    return check_files(args, lambda path: lint_file(path, args.rules))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="ryspec", description="Tools for ryspec documents.")
    commands = parser.add_subparsers(dest="command", required=True, metavar="command")

    validate = commands.add_parser(
        "validate",
        help="check TOML documents against the ryspec schema",
        description="Check every .toml file under each PATH: first as TOML, then against "
        "the ryspec JSON Schema. A file's own #:schema header names its schema, "
        "unless --schema is given; with neither, the schema this package ships is used.",
    )
    validate.add_argument("paths", nargs="*", type=Path, default=[Path(".")], metavar="PATH",
                          help="a file or directory to check (default: the current directory)")
    validate.add_argument("--schema", type=Path, metavar="FILE",
                          help="validate against FILE, ignoring #:schema headers")
    validate.add_argument("-q", "--quiet", action="store_true",
                          help="report only invalid files, then the summary")
    validate.set_defaults(handler=run_validate)

    lint = commands.add_parser(
        "lint",
        help="check TOML documents against the schema and the rules it cannot check",
        description="Check every .toml file under each PATH with libryspec, the C "
        "library, layer by layer: its TOML, the grammar of expression form, the ryspec "
        "schema (libryspec's own copy of it, not jsonschema), then the rules of "
        "SPEC.md's \"What the schema cannot check\", stopping at a file's first "
        "violation.",
    )
    lint.add_argument("paths", nargs="*", type=Path, default=[Path(".")], metavar="PATH",
                      help="a file or directory to check (default: the current directory)")
    lint.add_argument("-r", "--rule", dest="rules", type=int, action="append", metavar="N",
                      help="check rule N of SPEC.md alone, or 0 for the schema; repeat for "
                      "several, which run in the order given (default: the schema, then every "
                      "rule libryspec checks). A rule presumes a document the schema accepts")
    lint.add_argument("-q", "--quiet", action="store_true",
                      help="report only failing files, then the summary")
    lint.set_defaults(handler=run_lint)

    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    return args.handler(args)
