"""Hold libryspec's copy of the schema to the JSON Schema, by mutation.

Every `key = value` line of every document under the given paths is replaced
in turn by each of a set of wrong values, or deleted, and each mutant that is
still TOML is judged twice: by jsonschema against schemas/v0/, as
`ryspec validate` judges it, and by libryspec's parser and linter, as
`ryspec lint` does, whose schema check runs before any rule: a rule's
violation is the schema passed. The two must agree, but for one difference
by design: the schema takes any non-blank `(...)` string as an expression,
and only the parser reads the grammar inside it, so a mutant jsonschema
accepts may fail libryspec with a grammar error.

Exits 1 when any mutant is judged otherwise -- rejected by jsonschema and
passed by libryspec, or passed by jsonschema and refused by libryspec for any
reason but its grammar -- and 0 when none is. Needs the ryspec package with
its extension: installed (`make install`), or on PYTHONPATH.

    python3 python/tools/schema_fuzz.py [PATH ...]
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import tempfile
from collections import Counter
from collections.abc import Iterator
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path

import tomli

from ryspec import _core
from ryspec.validate import load_validator

REPOSITORY = Path(__file__).resolve().parents[2]

# What each value is replaced by: wrong types, malformed names, broken rules,
# bounds and literals, values out of range, and the values TOML has that JSON
# does not. Add to it when the schema grows a constraint nothing here meets.
MUTATIONS = [
    "1", "-1", "0", "1.5", "2.0", "1e300", "true", "nan", "inf", "-0.0",
    '"x"', '"x y"', '"a.b"', '"a..b"', '"1a"', '""', '"functional\\u00a0safety"',
    '"bool"', '"number"', '"text"', '"s"', '"min"', '"json"', '"flatbuffers"',
    "[]", '["x"]', '["x", "x"]', '["a.b"]', "[1]",
    "{}", "{ value = 1 }", '{ value = "x" }', "{ value = true }", '{ qvar = "s" }',
    '{ qvars = ["s"] }', "{ max = -1 }", "{ min = 1, max = 0 }", '{ max = "p" }',
    '{ time_unit = "s" }', '{ max = 1, time_unit = "y" }',
    '"(x)"', '"({x})"', '"({x}) "', '"( )"',
    '["not"]', '["not", "x", "y"]', '["and", "x"]', '["once", "x", "y"]',
    '["once", "x", { max = 1 }, { max = 2 }]', '["since", "x"]',
    '["lt", "a.b", "c"]', '["lt", "a", { value = "x" }]', '["eq", "x", { value = true }]',
    '["contains", "x", { value = 1 }]', '["assign", "x", "y"]',
    '["forall", "x", { qvars = [] }]', '["sometimes", "x"]', "[1, 2]",
    "1979-05-27", "07:32:00", "1979-05-27T07:32:00Z",
]
DELETE = None  # the line removed, as a mutation

ASSIGNMENT = re.compile(r"^(\s*[A-Za-z_\"][\w.\" -]*?\s*=\s*)(.*)$")


@dataclass(frozen=True)
class Mutant:
    source: Path
    line: int  # 1-based
    mutation: str | None
    text: str

    def __str__(self) -> str:
        what = "deleted" if self.mutation is None else f"= {self.mutation}"
        return f"{self.source}:{self.line}: {what}"


def toml_files(paths: list[Path]) -> Iterator[Path]:
    for path in paths:
        if path.is_dir():
            yield from sorted(path.rglob("*.toml"))
        else:
            yield path


def mutants(path: Path) -> Iterator[Mutant]:
    """Each mutant of the document at path: one assignment's value replaced
    by each mutation, or the assignment deleted."""
    lines = path.read_text(encoding="utf-8").splitlines()
    for i, line in enumerate(lines):
        match = ASSIGNMENT.match(line)
        if not match or line.lstrip().startswith("#"):
            continue
        for mutation in [*MUTATIONS, DELETE]:
            changed = lines[:]
            changed[i] = "" if mutation is None else match.group(1) + mutation
            yield Mutant(path, i + 1, mutation, "\n".join(changed) + "\n")


@dataclass
class Verdicts:
    """What judging the mutants of some documents found."""

    counts: Counter[tuple[bool, str]] = field(default_factory=Counter)
    skipped: int = 0  # not TOML to tomli, so no schema verdict
    missed: list[tuple[str, str]] = field(default_factory=list)   # jsonschema rejects, libryspec passes
    refused: list[tuple[str, str]] = field(default_factory=list)  # jsonschema accepts, libryspec refuses

    def add(self, other: Verdicts) -> None:
        self.counts += other.counts
        self.skipped += other.skipped
        self.missed += other.missed
        self.refused += other.refused


def judge(path: Path, schema: Path) -> Verdicts:
    """Judge every mutant of the document at path, twice."""
    validator = load_validator(schema)
    found = Verdicts()
    with tempfile.TemporaryDirectory() as tmp:
        scratch = Path(tmp) / "mutant.toml"
        for mutant in mutants(path):
            try:
                data = tomli.loads(mutant.text)
            except tomli.TOMLDecodeError:
                found.skipped += 1
                continue
            error = next(iter(validator.iter_errors(data)), None)
            scratch.write_text(mutant.text, encoding="utf-8")
            finding = next(iter(_core.lint(str(scratch))), None)
            kind = finding[0] if finding else "ok"
            if kind == "semantic":
                finding, kind = None, "ok"  # the schema passed, then a rule failed
            found.counts[(error is None, kind)] += 1
            if error is not None and finding is None:
                found.missed.append((str(mutant), error.message))
            elif error is None and finding is not None and kind != "grammar":
                found.refused.append((str(mutant), f"{kind} error: {finding[3]}"))
    return found


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Hold libryspec's schema check to the JSON Schema, by mutation."
    )
    parser.add_argument("paths", nargs="*", type=Path, metavar="PATH",
                        default=[REPOSITORY / "examples", REPOSITORY / "data" / "valid"],
                        help="documents to mutate, or directories of them "
                        "(default: examples/ and data/valid/)")
    parser.add_argument("--schema", type=Path, metavar="FILE",
                        default=REPOSITORY / "schemas" / "v0" / "ryspec.schema.json",
                        help="the JSON Schema (default: schemas/v0/ryspec.schema.json)")
    parser.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 1, metavar="N",
                        help="judge N documents at once (default: one per CPU)")
    parser.add_argument("--show", type=int, default=20, metavar="N",
                        help="show at most N disagreements of each kind (default: 20)")
    args = parser.parse_args(argv)

    schema = args.schema.resolve()
    files = list(toml_files(args.paths))
    found = Verdicts()
    with ProcessPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        for verdicts in pool.map(judge, files, [schema] * len(files)):
            found.add(verdicts)

    print(f"{sum(found.counts.values())} mutants of {len(files)} documents judged, "
          f"{found.skipped} not TOML to tomli")
    print(f"  {'jsonschema':<12}{'libryspec':<12}mutants")
    for (accepted, kind), count in sorted(found.counts.items(), key=lambda kv: -kv[1]):
        print(f"  {'accepts' if accepted else 'rejects':<12}{kind:<12}{count}")

    for title, disagreements in [
        ("rejected by jsonschema, passed by libryspec", found.missed),
        ("passed by jsonschema, refused by libryspec", found.refused),
    ]:
        if disagreements:
            print(f"\n{len(disagreements)} {title}:")
            for mutant, why in disagreements[: args.show]:
                print(f"  {mutant}\n    {why}")
    return 1 if found.missed or found.refused else 0


if __name__ == "__main__":
    sys.exit(main())
