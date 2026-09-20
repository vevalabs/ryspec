"""What has to ship with the package, as against only with the repo.

Two things, and a wheel missing either imports cleanly and fails later.
DEFAULT_SCHEMA resolves inside the installed package, and in a checkout it gets
there through a symlink a wheel cannot carry, so CMake has to install the
schema to the same place the symlink points. And `ryspec.semantics` is a
binding over libryspec, so the library has to be found -- from beside the
package in a wheel, or from the repository's build tree in a checkout.
"""

from __future__ import annotations

import re
from pathlib import Path

import ryspec
from ryspec import DEFAULT_SCHEMA

REPO_ROOT = Path(__file__).resolve().parents[2]
# Taken from the module rather than from DEFAULT_SCHEMA, whose resolved path
# leaves the package through the schemas symlink and lands wherever the
# checkout happens to be.
PACKAGE_DIR = Path(ryspec.__file__).resolve().parent


def test_default_schema_lives_inside_the_package():
    assert DEFAULT_SCHEMA.exists()
    assert DEFAULT_SCHEMA.is_relative_to(PACKAGE_DIR)


def test_the_library_is_found():
    """The binding has something to bind to.

    In a checkout this is build/libryspec.so, which `make` puts there and the
    session fixture builds if it is missing; in a wheel it is the copy CMake
    installed into the package. Either way ryspec.semantics resolves it, and the
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


def test_cmake_installs_the_schema_where_the_package_reads_it():
    """The wheel's half of the schema, which the suite cannot otherwise see.

    In a checkout DEFAULT_SCHEMA resolves through python/src/ryspec/schemas, a
    symlink to the schemas at the repository root. A wheel is built by walking
    the package directory, which does not follow that symlink, so CMake
    installs the schema instead -- and the two have to agree on where it lands.
    Nothing else pins them together, and a wheel with the schema in the wrong
    place still imports.
    """
    rule = re.search(
        r"install\(DIRECTORY\s+(\S+)\s+DESTINATION\s+(\S+)\)",
        (REPO_ROOT / "CMakeLists.txt").read_text(),
    )
    assert rule, "CMakeLists.txt installs no directory into the package"

    source, destination = Path(rule.group(1)), Path(rule.group(2))
    # The destination is relative to the wheel root, so its first component is
    # the package itself; the rest is where the schema sits inside the package.
    installed = destination.relative_to(ryspec.__name__) / source.name

    assert DEFAULT_SCHEMA.relative_to(PACKAGE_DIR).is_relative_to(installed), (
        f"CMake installs {source} to {destination}, which is not where "
        f"DEFAULT_SCHEMA reads it from ({DEFAULT_SCHEMA.relative_to(PACKAGE_DIR)})"
    )
