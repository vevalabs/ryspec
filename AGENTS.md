# Agent Guide

`ryspec` is a declarative temporal-logic specification format based on a subset of TOML.

## Layout

The repository is the format: its JSON Schema, the language definition, and the
corpus that holds the schema to it, plus a C library that reads documents.

| path | holds |
| --- | --- |
| `schemas/v0/` | the JSON Schema, the authority on what shape a document may have. `libryspec` holds a copy of it by hand (`libs/ryspec/src/lint_check.c`), and a change here is owed one there |
| `SPEC.md` | the prose definition: expression syntax, semantics, and the rules the schema cannot check |
| `data/`, `examples/` | the document corpus and the documented examples |
| `data/README.md` | what each corpus directory means, and what a file's `#:expect-*-error` header still asserts |
| `data/semantic/` | one schema-valid fixture per rule in `SPEC.md`'s "What the schema cannot check" |
| `libs/ryspec/` | `libryspec`, a static C library built on tomlc17 (fetched by CMake) and a hand-written expression parser, with the public header `include/ryspec/ryspec.h`, its sources and its tests. The root `CMakeLists.txt` adds it. A thin wrapper: a document is tomlc17's tree behind an opaque `ryspec_toml_doc`, with each expression-form rule replaced by its prefix-form twin as the document is parsed, so every rule is a prefix-form value. The public header names no tomlc17 type, and has no reader of values: a document is indexed as it is parsed, and validated, its entities read through it (`ryspec_entity_*`) by kind, name, parent and place, never their values; rule bodies and TOML values stay private. It allocates nothing but through tomlc17's allocator, which the `allocations` test holds it to |
| `libs/ryspec/src/toml_doc.h`, `src/toml_doc.c` | documents: tomlc17's result in a block of its allocator. `toml_doc.h` is where tomlc17's types enter, and it and the private headers built on it, `src/lint.h` among them, are all that name them; and the index: the document's entities -- namespaces, rules, properties, `given`s, `check`s, private rules, variables, monitors -- in one table, one id space in document order, `parent` their one structural link; a name index by (scope, name), the specification's symbol space, which monitors are outside; and the resolver of SPEC.md's "Names" over it. The index is fields of the document, built once its expressions are translated and released with it; the linter reads it. The public `ryspec_entity_*` functions, which take the document, are its read-only face, total over any id |
| `libs/ryspec/src/expr_parser.h`, `src/expr_parser.c` | the expression parser: recursive descent over SPEC.md's grammar, emitting a rule as private events in prefix form's order, written as prefix-form TOML; with the operators and the TOML text writer the library shares |
| `libs/ryspec/src/lint.h`, `src/lint.c`, `src/lint_check.c` | the linter: over the index, a walker over prefix-form rules, and one check per rule of SPEC.md's "What the schema cannot check", and the schema, by hand, as check 0, dispatched by rule number so each runs alone (`ryspec_toml_lint_rule()`) or all in order (`ryspec_toml_lint()`), stopping at the first violation. Nothing under `extras` is checked. `lint.h` and `lint.c` hold the walker and the dispatch table, and `lint_check.c` holds the checks, a section for each kind of thing they read |
| `libs/ryspec/src/translate.c` | expressions into prefix form: each expression-form string at a rule position is written as prefix-form TOML, parsed by tomlc17, and spliced into the document's tree in its place, placed where the expression spells each part. A malformed expression fails the parse with a grammar error |
| `python/tools/schema_fuzz.py` | the schema fuzzer (`make fuzz`): mutates every value of `examples/` and `data/valid/` and holds `libryspec`'s schema check to jsonschema on each mutant. Run it after changing either: a disagreement is a bug in one of the two, and a constraint no mutation meets wants one added to `MUTATIONS` |
| `python/` | the `ryspec` Python package: `ryspec validate` checks documents against the schema, and `ryspec lint` (`lint.py`) has `libryspec` parse and lint documents -- TOML, grammar, its own copy of the schema, then the rules -- reporting a file's first violation; `--rule N` runs single checks. Its extension, `ryspec._core` (`python/ext/`, built by CMake from the root `pyproject.toml`), binds `libryspec`: `lint(path, rules=None)`, `lint_rules()` and `index(path)` |

## Rules

Rules have two representations: expression syntax and prefix syntax.

- Prefix syntax is authoritative.
- A prefix rule has the structure `[opcode, args, kwargs]`.
- Expression syntax is a human-friendly representation of the same rule semantics.
- Expression syntax may temporarily lag behind prefix syntax, but should converge toward feature parity.
- Expression syntax must never be more expressive than prefix syntax.
- Any capability supported by expression syntax must have a corresponding prefix representation.

## Properties

Properties define externally meaningful requirements or assertions.

- A property has a `given` condition and a `check` condition.
- `given -> check` has implication semantics: whenever `given` holds, `check` is required to hold.
- `check` defines the actual requirement whose satisfaction is evaluated.
- Property metadata such as `title`, `description`, and `tags` describes the property without changing its logical semantics.

## Namespaces

Namespaces provide a packaging mechanism for properties.

- A namespace groups related rules and properties without changing their semantics.
- `[rules]` and `[properties]` at the root are the anonymous namespace. A named namespace is a path under `[namespace]`, and it holds its own `rules` and `properties`: `[namespace.a.b.properties.p]` is the property `p` of the namespace `a.b`, referenced as `a.b.p`.
- A named namespace is either a leaf, holding `rules`, `properties` and `extras`, or intermediate, holding nested namespaces alone. No namespace is named `rules`, `properties` or `extras`.
- Namespaces are organizational units rather than runtime scopes.
- Properties can therefore be referenced independently of how they are packaged.
- The namespace hierarchy provides a natural mechanism for organizing larger specifications and avoiding name collisions.

## Monitors

- Monitors are independent of namespaces.
- A monitor describes a runtime interface rather than a logical package.
- Its inputs, outputs, parameters, and runtime configuration remain separate from the organization of properties.
- A document can consequently define multiple monitor interfaces over the same collection of properties.
- `monitors` is a table keyed by name, `[monitors.<name>]`, and like `[variables]` it sits at the root alone: no namespace holds a monitor.

## Temporal Bounds

- Temporal bounds have explicit unit semantics with hierarchical defaults.
- A bound may specify its own unit.
- If a bound does not specify a unit, the property's time unit is used.
- If the property does not specify a time unit, the monitor's runtime time unit is used.
- If the monitor does not specify one either, the bound counts steps.

## Guidelines

* Every `ryspec` document must conform to valid TOML 1.1 syntax.
* Every `ryspec` document must conform to the `ryspec` JSON Schema.
* Preserve the distinction between TOML syntax, ryspec language syntax, and document-level schema constraints.
* Invalid syntax and unsupported constructs should produce clear diagnostics rather than being silently ignored.
* A corpus file declaring `#:expect-grammar-error` is owed a rejection by
  `libryspec` when it parses the file, and one declaring
  `#:expect-semantic-error` by `libryspec`'s linter, once it checks the
  file's rule; what each asserts besides is that the schema accepts it. Keep
  both the file and its marker.

## Code style

- C23, `snake_case`, `ryspec_` prefix on all public symbols, `RYSPEC_` on macros/constants.
- 2-space indentation, no tabs (see `.editorconfig`).