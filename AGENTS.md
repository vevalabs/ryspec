# Agent Guide

`ryspec` is a declarative temporal-logic specification format based on a subset of TOML.

## Layout

The repository root is the C project -- the tree-sitter grammar and the parser
generated from it. Everything else hangs off it.

| path | holds |
| --- | --- |
| `src/grammar.json` | the grammar, authored by hand |
| `CMakeLists.txt`, `Makefile`, `src/`, `test/`, `queries/` | the parser: `make`, `make test` |
| `cli/ryspec-parse.c` | the application over the parser, built when a tree-sitter runtime is available |
| `src/database.h`, `src/database.c`, `src/loader.c`, `src/semantics.c` | the in-memory database over the parse tree: one namespaced name space, deduplicated rules and strings, and the loader's checks. Part of `libryspec` when a runtime is available, and out of the build with `ryspec-parse` when none is |
| `cli/ryspec-db.c` | the application over the database |
| `schemas/v0/` | the JSON Schema |
| `DESIGN.md` | why the format and the code are shaped as they are, and the tensions still open |
| `data/`, `examples/` | the document corpus and the documented examples |
| `python/src/ryspec` | the `ryspec` validator: `jsonschema` for the schema layer, and a `ctypes` binding over `libryspec` for the loader's. It holds no checks of its own |
| `pyproject.toml` | the whole of the package, including how it is built: py-build-cmake runs `CMakeLists.txt` with `RYSPEC_PYTHON_MODULE=ON`, so `pip install .` compiles the same C `make` does. There is no `setup.py` |
| `python/tests` | the Python test suite: `pytest` from the root |

## Guidelines

* Every `ryspec` document must conform to valid TOML 1.1 syntax.
* Every `ryspec` document must conform to the `ryspec` JSON Schema.
* Every `ryspec` document must be parseable by the ryspec tree-sitter grammar.
* The tree-sitter grammar defines the finest syntactic and semantic structure of `ryspec` documents.
* Preserve the distinction between TOML syntax, ryspec language syntax, and document-level schema constraints.
* Invalid syntax and unsupported constructs should produce clear diagnostics rather than being silently ignored.
* TOML datetime structs will be parsed as plain strings.
* A document passes three layers, and each is implemented once. The grammar's
  is `src/`, reported by `ryspec-parse`. The schema's is `schemas/v0/`, applied
  by `jsonschema` in the Python package. The loader's is `src/semantics.c`,
  reported by `ryspec-db` and by `ryspec validate` alike -- the Python package
  binds it and does not reimplement it. A check belongs to exactly one layer,
  and a layer to exactly one implementation.


## The Grammar

* `[extras]` takes arbitrary keys and accepts a subset of TOML. We do not support dotted name for individual tables and datetime functionality in TOML.
* 


## Syntax Rules

* Do not mix past and future temporal logic rules.
* `properties.given` and `properties.check` only accept past temporal logic  rules.
* `properties.impose` only accept future temporal logic rules.

## Code style

- C23, `snake_case`, `ryspec_` prefix on all public symbols, `RYSPEC_` on
  macros/constants.
- 2-space indentation, no tabs (see `.editorconfig`).
- `src/database.h` is the installed interface and everything in it is exported.
  `src/internal.h` is what the three database sources share and nothing else:
  it marks its declarations hidden, so the library exports exactly what the
  installed header promises.
- `cli/ryspec-parse.c` predates this and uses CamelCase types and four-space
  indentation. Leave it as it is; new code follows the above.
- The tree-sitter runtime is fetched and linked statically; no copy of it is in
  this repository. Bumping it is editing `TREE_SITTER_VERSION` and
  `TREE_SITTER_SHA256` in `CMakeLists.txt`. Nothing else records the version.
  Run the suite afterwards -- `src/parser.c` is generated at language ABI 14
  and the runtime has to still speak it.
- `ryspec.semantics` binds seven functions of `src/database.h` by signature and
  mirrors no struct, so adding a field to one breaks nothing. Changing one of
  those seven signatures means changing the binding in the same commit.