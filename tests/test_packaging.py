"""The schema ships with the package, not just with the repo.

DEFAULT_SCHEMA resolves inside the installed package, so a wheel that omits
schemas/v0/ryspec.schema.json still imports cleanly and only fails when
someone runs `validate` -- these tests check the declared package-data
patterns actually match it.
"""

from __future__ import annotations

import glob
import os
import tomllib
from pathlib import Path

from ryspec import DEFAULT_SCHEMA

REPO_ROOT = Path(__file__).resolve().parent.parent
PACKAGE_DIR = Path(DEFAULT_SCHEMA).resolve().parent.parent.parent


def _package_data_patterns() -> list[str]:
    pyproject = tomllib.loads((REPO_ROOT / "pyproject.toml").read_text())
    return pyproject["tool"]["setuptools"]["package-data"]["ryspec"]


def test_default_schema_lives_inside_the_package():
    assert DEFAULT_SCHEMA.exists()
    assert PACKAGE_DIR.name == "ryspec"


def test_package_data_patterns_match_the_default_schema():
    # setuptools globs package-data relative to the package directory; a
    # pattern such as "schemas/**.json" reads as recursive but glob treats
    # ** inside a path segment as a plain *, silently matching nothing.
    matched = set()
    for pattern in _package_data_patterns():
        matched.update(
            (PACKAGE_DIR / hit).resolve()
            for hit in glob.glob(pattern, root_dir=os.fspath(PACKAGE_DIR), recursive=True)
        )

    assert DEFAULT_SCHEMA.resolve() in matched, (
        f"no package-data pattern matches {DEFAULT_SCHEMA.name}; "
        f"patterns={_package_data_patterns()}, matched={sorted(matched)}"
    )
