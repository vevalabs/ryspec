"""What has to ship with the package, as against only with the repo.

Two things, and a wheel missing either imports cleanly and fails later.
DEFAULT_SCHEMA resolves inside the installed package, so the declared
package-data patterns have to match it. And `ryspec.semantics` is a binding
over libryspec, so the library has to be found -- from beside the package in a
wheel, or from the repository's build tree in a checkout.
"""

from __future__ import annotations

import glob
import os
import tomllib
from pathlib import Path

import ryspec
from ryspec import DEFAULT_SCHEMA

REPO_ROOT = Path(__file__).resolve().parents[2]
# Where setuptools globs package-data from. Taken from the module rather than
# from DEFAULT_SCHEMA, whose resolved path leaves the package through the
# schemas symlink and lands wherever the checkout happens to be.
PACKAGE_DIR = Path(ryspec.__file__).resolve().parent


def _package_data_patterns() -> list[str]:
    pyproject = tomllib.loads((REPO_ROOT / "pyproject.toml").read_text())
    return pyproject["tool"]["setuptools"]["package-data"]["ryspec"]


def test_default_schema_lives_inside_the_package():
    assert DEFAULT_SCHEMA.exists()
    assert DEFAULT_SCHEMA.is_relative_to(PACKAGE_DIR)


def test_the_library_is_found():
    """The binding has something to bind to.

    In a checkout this is build/libryspec.so, which `make` puts there and the
    session fixture builds if it is missing; in a wheel it is the copy setup.py
    compiled into the package. Either way ryspec.semantics resolves it, and the
    error when it cannot names everywhere it looked.
    """
    from ryspec.semantics import library_path

    assert library_path().exists()


def test_the_library_answers():
    """A round trip through ctypes, so a stale or mismatched build is caught.

    The document trips exactly one loader check, and the wording is the C
    side's, so this fails if the two drift apart.
    """
    from ryspec.semantics import first_semantic_error

    error = first_semantic_error(
        'version = 0\n[[properties]]\nname = "p"\ncheck = ["once", "q", {min = 30, max = 2}]\n'
    )
    assert error is not None and "is greater than max" in error, error


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
