# Agent Guide

`ryspec` is a declarative temporal-logic specification format based on a subset of TOML.

## Layout

The repository is the format: its JSON Schema, the language definition, and the
corpus that holds the schema to it, plus a C library skeleton that reads documents.

| path | holds |
| --- | --- |
| `schemas/v0/` | the JSON Schema. It is the one implemented layer, so it is the authority on what a document may be |
| `SPEC.md` | the prose definition: expression syntax, semantics, and the rules the schema cannot check |
| `data/`, `examples/` | the document corpus and the documented examples |
| `data/README.md` | what each corpus directory means, and what a file's `#:expect-*-error` header still asserts |
| `data/semantic/` | one schema-valid fixture per rule in `SPEC.md`'s "What the schema cannot check" |
| `CMakeLists.txt`, `include/`, `src/`, `tests/` | `libryspec`, a static C library built on tomlc17 (fetched by CMake) |
| `python/` | the `ryspec` Python package, whose `ryspec validate` checks documents against the schema, and `ryspec lint` against the rules the schema cannot check, through `libryspec` (`python/ext/`, built by CMake from the root `pyproject.toml`) |

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
* A corpus file declaring `#:expect-semantic-error` or `#:expect-grammar-error`
  is owed a rejection by nobody today, and what it asserts instead is that the
  schema accepts it. Keep both the file and its marker.

## Code style

- The schema is edited at `schemas/v0/`, and every corpus file and example points at it through its `#:schema` header, as a path relative to the file.
