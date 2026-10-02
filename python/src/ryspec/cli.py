"""The `ryspec` command line."""

from __future__ import annotations

import argparse
import sys
from collections.abc import Callable, Iterable, Iterator
from pathlib import Path

from ryspec.validate import Diagnostic, validate_file


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
    """Run `check` over every file under `args.paths`, and report as `validate`
    and `lint` both do."""
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
    return check_files(args, lambda path: validate_file(path, args.schema))


def run_lint(args: argparse.Namespace) -> int:
    from ryspec.lint import lint_file

    return check_files(args, lint_file)


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
        help="check TOML documents against the rules the schema cannot check",
        description="Check every .toml file under each PATH with libryspec, the C "
        "library, against the rules of SPEC.md's \"What the schema cannot check\". "
        "No rule is implemented yet, so every document that parses passes.",
    )
    lint.add_argument("paths", nargs="*", type=Path, default=[Path(".")], metavar="PATH",
                      help="a file or directory to check (default: the current directory)")
    lint.add_argument("-q", "--quiet", action="store_true",
                      help="report only failing files, then the summary")
    lint.set_defaults(handler=run_lint)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    return args.handler(args)
