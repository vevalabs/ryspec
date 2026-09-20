# tree-sitter runtime

The [tree-sitter][] runtime, at the tag in `VERSION`, vendored rather than
fetched. MIT licensed; `LICENSE` is upstream's, and `src/unicode/` carries ICU's
beside it.

It is here because the Python package compiles it. A wheel is built from an
sdist, an sdist cannot download anything, and the database needs a runtime to
walk a tree -- so the runtime has to be in the tree. CMake uses this same copy,
which is the point: `make` and `pip install .` compile identical source, and
neither needs the network.

Only what `src/lib.c` pulls in is here. The Rust, WASM and editor bindings, the
WASM stdlib and the test suite are no part of it.

Nothing here is edited. To move to another release, run

```sh
scripts/vendor-tree-sitter.sh v0.27.1
```

which rewrites `VERSION` beside this file, and that is where CMake reads the
tag -- nothing else records it. A runtime is only
obliged to load the language ABIs it was built for, so the version that matters
is the one `src/parser.c` is generated at: ABI 14, which this runtime speaks.

A tree-sitter runtime already installed on the host is used in preference to
this one, so a distribution packaging `ryspec` against its own copy gets what it
expects. Pass `-DRYSPEC_VENDORED_TREE_SITTER=ON` to compile this one regardless.

[tree-sitter]: https://github.com/tree-sitter/tree-sitter
