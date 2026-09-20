"""The loader's checks, from the C library.

Everything README.md lists under "What the schema cannot check" is decided by
`libryspec`, which reads a document's parse tree rather than its decoded value:
the names it declares, the rule each one stands for, and whether the two hold
together. This module is the binding, and holds no rules of its own. The C
side is `src/semantics.c`, and `ryspec-db` is the same checks from the
command line.

    from ryspec.semantics import first_semantic_error
    first_semantic_error(path.read_text(), path)

Two layers are deliberately not here. The schema's is `jsonschema`'s, in
`ryspec.__init__`. The grammar's is `ryspec-parse`'s: a document that does not
parse reports nothing here, which is what lets `ryspec validate
data/malformed` pass and mean it.

The library is found rather than built. A wheel carries its own copy beside
this file; a checkout has the one CMake builds into `build/`; `$RYSPEC_LIBRARY`
overrides both.
"""

from __future__ import annotations

import ctypes
import ctypes.util
import os
from pathlib import Path

__all__ = ["LibraryNotFound", "all_semantic_errors", "first_semantic_error", "library_path"]

_PACKAGE_DIR = Path(__file__).resolve().parent
# python/src/ryspec -> the repository root, where CMake's build/ sits. Present
# in a checkout and meaningless in a wheel, where the copy beside this file is
# the one that answers.
_REPO_ROOT = _PACKAGE_DIR.parents[2]
_LIBRARY_NAME = "libryspec.dll" if os.name == "nt" else "libryspec.so"


class LibraryNotFound(RuntimeError):
    """libryspec could not be found, so the loader's checks cannot run."""


def _candidates() -> list[Path]:
    override = os.environ.get("RYSPEC_LIBRARY")
    found = [Path(override)] if override else []
    # Beside this file: what CMake installs into the package, and what a wheel
    # carries. First, so an installed package never reaches out of itself.
    found.append(_PACKAGE_DIR / _LIBRARY_NAME)
    # The repository's own build tree, so the test suite and anyone working in
    # a checkout gets the library they just compiled.
    found.append(_REPO_ROOT / "build" / _LIBRARY_NAME)
    system = ctypes.util.find_library("ryspec")
    if system:
        found.append(Path(system))
    return found


def library_path() -> Path:
    """Where the library was found. Raises LibraryNotFound if it was not."""
    for candidate in _candidates():
        if candidate.exists():
            return candidate
    # ctypes.util.find_library returns a bare soname on some platforms, which
    # no path test can confirm; let the loader have the last word on it.
    raise LibraryNotFound(
        f"cannot find {_LIBRARY_NAME}. Looked at: "
        + ", ".join(str(candidate) for candidate in _candidates())
        + ". Build it with `make`, or set RYSPEC_LIBRARY."
    )


# Seven entry points, all taking and returning scalars and bytes. Nothing here
# mirrors a struct from database.h, so the binding does not have to be kept in
# step with a layout -- only with these signatures.
_SIGNATURES = {
    "ryspec_database_new": ([], ctypes.c_void_p),
    "ryspec_database_free": ([ctypes.c_void_p], None),
    "ryspec_database_add": (
        [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t],
        ctypes.c_uint32,
    ),
    "ryspec_document_parsed": ([ctypes.c_void_p, ctypes.c_uint32], ctypes.c_bool),
    "ryspec_database_check": ([ctypes.c_void_p], ctypes.c_uint32),
    "ryspec_diagnostic_count": ([ctypes.c_void_p], ctypes.c_uint32),
    "ryspec_diagnostic_format": (
        [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_char_p, ctypes.c_size_t],
        ctypes.c_size_t,
    ),
}

_library = None


def _load() -> ctypes.CDLL:
    global _library
    if _library is None:
        library = ctypes.CDLL(str(library_path()))
        for name, (argtypes, restype) in _SIGNATURES.items():
            function = getattr(library, name)
            function.argtypes = argtypes
            function.restype = restype
        _library = library
    return _library


def all_semantic_errors(source: str | bytes, path: str | os.PathLike = "<document>") -> list[str]:
    """Every loader diagnostic `source` carries, as `line:column: message`.

    `path` names the document in nothing but the diagnostics, and is trimmed
    back off them, so a caller that already knows the file can print it once.

    A document the grammar rejects yields no diagnostics: it declares no name
    for the checks to reach, and its fault belongs to a layer this one sits
    above.
    """
    if isinstance(source, dict):
        raise TypeError(
            "all_semantic_errors takes the document's source, not its decoded value: "
            "the checks read a parse tree, so they need the text"
        )
    library = _load()
    if isinstance(source, str):
        source = source.encode()
    name = os.fspath(path).encode() if not isinstance(path, str) else path.encode()

    db = library.ryspec_database_new()
    if not db:
        raise LibraryNotFound("the runtime cannot load the ryspec parser (ABI mismatch)")
    try:
        document = library.ryspec_database_add(db, name, source, len(source))
        if not library.ryspec_document_parsed(db, document):
            return []
        library.ryspec_database_check(db)

        prefix = name + b":"
        errors = []
        for index in range(library.ryspec_diagnostic_count(db)):
            size = library.ryspec_diagnostic_format(db, index, None, 0) + 1
            buffer = ctypes.create_string_buffer(size)
            library.ryspec_diagnostic_format(db, index, buffer, size)
            report = buffer.value
            errors.append(report.removeprefix(prefix).decode())
        return errors
    finally:
        library.ryspec_database_free(db)


def first_semantic_error(source: str | bytes, path: str | os.PathLike = "<document>") -> str | None:
    """The first loader diagnostic `source` carries, or None if it holds none."""
    errors = all_semantic_errors(source, path)
    return errors[0] if errors else None
