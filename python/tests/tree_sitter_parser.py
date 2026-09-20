"""Build and load the ryspec tree-sitter parser.

The parser is compiled by the repository's own CMake project, into the same
build directory whether it is a test or a person running cmake by hand.
"""

from __future__ import annotations

import ctypes
import shutil
import subprocess
import warnings
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
BUILD_DIR = REPO_ROOT / "build"
LIBRARY = BUILD_DIR / "libryspec.so"


def build() -> Path:
    """Compile the parser if it is out of date, and return the shared library."""
    if shutil.which("cmake") is None:
        raise RuntimeError("cmake is needed to build the ryspec parser")
    # A build tree that already exists is configured again without any -D, so
    # whatever cache a person or the Makefile put there survives; a fresh one
    # gets the Makefile's own default build type rather than none at all.
    # Reconfiguring a current tree is a no-op, and both steps are quiet unless
    # they fail, where the output rides along in the CalledProcessError that
    # the caller turns into a skip.
    configure = ["cmake", "-S", str(REPO_ROOT), "-B", str(BUILD_DIR)]
    if not (BUILD_DIR / "CMakeCache.txt").exists():
        configure.append("-DCMAKE_BUILD_TYPE=Release")
    for command in (configure, ["cmake", "--build", str(BUILD_DIR), "--target", "ryspec"]):
        subprocess.run(command, check=True, capture_output=True)
    return LIBRARY


def load_language():
    """Load the compiled parser as a tree_sitter.Language."""
    import tree_sitter

    library = ctypes.cdll.LoadLibrary(str(build()))
    library.tree_sitter_ryspec.restype = ctypes.c_void_p
    with warnings.catch_warnings():
        # ctypes hands us a raw pointer rather than the capsule a generated
        # binding would; tree_sitter still accepts it, under a warning.
        warnings.simplefilter("ignore", DeprecationWarning)
        return tree_sitter.Language(library.tree_sitter_ryspec())


def first_error(node):
    """The first ERROR or MISSING node under `node`, or None.

    `Node.children` does not reach a missing hidden token, so a tree can carry
    an error this walk cannot name; callers test `has_error` for the verdict
    and use this only to report where.
    """
    if node.type == "ERROR" or node.is_missing:
        return node
    if not node.has_error:
        return None
    for child in node.children:
        found = first_error(child)
        if found is not None:
            return found
    return None


def parse_error(parser, source: bytes) -> str | None:
    """Parse `source`, returning None if it is clean or a description if not."""
    tree = parser.parse(source)
    root = tree.root_node
    if not root.has_error:
        return None
    node = first_error(root)
    if node is None:
        return "parse error"
    row, column = node.start_point
    kind = "missing" if node.is_missing else "unexpected"
    return f"{kind} {node.type} at line {row + 1}, column {column + 1}"
