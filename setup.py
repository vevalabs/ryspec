"""Build libryspec into the Python package.

The package is a binding, not a reimplementation: `ryspec.semantics` loads the
same library `make` builds and calls into it. So the library has to be there,
and a wheel carries its own copy -- compiled here, from the C sources at the
root of this repository and the tree-sitter runtime vendored beside them.

That is why the runtime is vendored rather than fetched. A wheel is built from
an sdist, an sdist cannot download anything, and walking a parse tree needs a
runtime. CMake compiles the same files, so `make` and `pip install .` produce
the same library from the same source.

Everything else about the package is declared in pyproject.toml. This file
exists for the extension alone.
"""

from __future__ import annotations

import os
import tempfile

from setuptools import setup
from setuptools.command.build_ext import build_ext
from setuptools.extension import Extension

# src/parser.c is generated and the rest is written by hand; all of it is
# committed. vendor/tree-sitter/src/lib.c is the runtime's own amalgamation --
# it includes every other source, so one translation unit is the whole library.
SOURCES = [
    "src/parser.c",
    "src/scanner.c",
    "src/database.c",
    "src/loader.c",
    "src/semantics.c",
    "vendor/tree-sitter/src/lib.c",
]

INCLUDE_DIRS = [
    "src",                          # database.h, internal.h, tree_sitter/parser.h
    "vendor/tree-sitter/include",   # tree_sitter/api.h
    "vendor/tree-sitter/src",       # the runtime's own headers, for lib.c
]


def supported_standard(compiler) -> list[str]:
    """`-std` for the newest C this compiler admits, or nothing if it has none.

    CMake asks for C23 and AGENTS.md says the same. The GNU dialect of it,
    though, and not the strict one: CMake leaves C_EXTENSIONS on, so its build
    is `-std=gnu23`, and the vendored runtime needs what that turns on. Under
    strict `-std=c23` glibc hides `le16toh` behind _DEFAULT_SOURCE, the runtime
    picks up an implicit declaration instead, and the library it produces
    builds cleanly and then fails to load. Matching CMake is the point.

    A compiler that has not reached C23 spells it `gnu2x`; one that spells it
    neither way is left to its default, which everywhere this builds is new
    enough.
    """
    if os.name == "nt":
        return []
    for standard in ("gnu23", "gnu2x"):
        with tempfile.TemporaryDirectory() as directory:
            probe = os.path.join(directory, "probe.c")
            with open(probe, "w") as handle:
                handle.write("int main(void) { return 0; }\n")
            try:
                compiler.compile([probe], output_dir=directory,
                                 extra_postargs=[f"-std={standard}"])
                return [f"-std={standard}"]
            except Exception:
                continue
    return []


class build_shared_library(build_ext):
    """Produce a plain shared library rather than an importable extension.

    `ryspec.semantics` loads it with ctypes, so what it needs is a predictable
    name and no PyInit_. Everything else build_ext does is what we want: the
    right compiler, the platform's flags, and a wheel tagged as carrying
    compiled code, which this one does.
    """

    def get_ext_filename(self, fullname: str) -> str:
        parts = fullname.split(".")
        suffix = ".dll" if os.name == "nt" else ".so"
        return os.path.join(*parts[:-1], parts[-1] + suffix)

    def build_extensions(self) -> None:
        standard = supported_standard(self.compiler)
        for extension in self.extensions:
            extension.extra_compile_args = standard + list(extension.extra_compile_args or [])
        super().build_extensions()


setup(
    ext_modules=[
        Extension(
            "ryspec.libryspec",
            sources=SOURCES,
            include_dirs=INCLUDE_DIRS,
            # Without this the wheel would build but import nothing useful:
            # the package has no fallback for a missing library, by design.
            optional=False,
        )
    ],
    cmdclass={"build_ext": build_shared_library},
)
