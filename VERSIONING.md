# Versioning Guide

This project uses semantic versioning for the `ryspec` language grammar.

A version change must reflect changes to the language accepted or represented by the Tree-sitter grammar.

## Version Numbers

Versions follow:

`MAJOR.MINOR.PATCH`

### MAJOR

Increment the major version when making an incompatible change to the `ryspec` language.

Examples:

* Removing existing syntax.
* Changing the meaning of existing syntax in an incompatible way.
* Renaming or removing grammar nodes that consumers may depend on.
* Changing the structure of the syntax tree in a way that breaks existing consumers.
* Making previously valid documents invalid.

### MINOR

Increment the minor version when adding functionality in a backward-compatible way.

Examples:

* Adding new syntax.
* Adding new temporal operators.
* Adding new grammar rules or node types without changing existing constructs.
* Extending the grammar to accept additional valid `ryspec` documents.

Existing valid documents and existing syntax-tree structures should remain compatible.

### PATCH

Increment the patch version for backward-compatible fixes that do not change the `ryspec` language.

Examples:

* Fixing an incorrect grammar rule.
* Correcting node fields or metadata without changing the language.
* Improving error recovery.
* Updating build configuration or development tooling.
* Fixing generated parser artifacts.

## Version Bump Procedure

When changing the grammar:

1. Determine whether the change is a major, minor, or patch release.
2. Update the Tree-sitter grammar.
3. Regenerate the parser and generated artifacts.
4. Update the grammar version in the project metadata.
5. Update the JSON Schema when the document model changes.
6. Update `ryspec` examples affected by the change.
7. Add or update grammar tests.
8. Run the complete test suite.
9. Verify that existing valid `ryspec` documents remain valid unless the change is intentionally breaking.
10. Review the generated syntax tree for affected constructs.
11. Update the changelog.
12. Create the release tag using the same version.

## Grammar Compatibility

The grammar and its syntax tree are part of the public interface.

Consumers may depend on:

* Rule names.
* Named syntax nodes.
* Anonymous tokens.
* Node hierarchy.
* Fields.
* Child ordering.
* Tree structure.

Therefore, changing the syntax tree can be a breaking change even when the corresponding `ryspec` documents remain valid.

For example, changing:

```text
temporal_expression
  operator
  operand
```

to:

```text
expression
  temporal_operator
  expression
```

may require a major version bump if downstream tools depend on the original node structure.

## Database Compatibility

The library exports more than the grammar. Where a tree-sitter runtime is
present, `libryspec` also carries the in-memory database, and `src/database.h`
is installed alongside it.

That interface rides on the same soname, so it is versioned by the same rule.
Consumers may depend on:

* The function signatures in `src/database.h`.
* The layout of the structs it defines.
* The values of its enumerators, and the bit positions of `ryspec_role`.
* `RYSPEC_NONE` being slot 0 of every table.

A breaking change to any of these is a major version bump, for the same reason
a renamed node type is: both are the library's public interface, and a consumer
cannot tell from the soname which half moved.

Adding a function, or a field at the end of a struct a consumer only ever
receives a pointer to, is a minor bump. Reordering or removing either is major.

The Python package is one of those consumers. It binds seven of these functions
by signature and mirrors no struct, which is why a field added to
`ryspec_symbol` costs it nothing; a changed signature among those seven is a
break, and the binding moves in the same commit as the header.

## JSON Schema Compatibility

The JSON Schema and Tree-sitter grammar serve different layers of the format.

Tree-sitter defines the finest-grained syntactic and semantic structure of `ryspec` documents. The JSON Schema defines the document-level structure and constraints.

When a language change affects both layers, update both together.

A grammar change must not silently introduce constructs that are rejected by the JSON Schema, and a schema change must not require syntax that cannot be represented by the grammar.

## Tests

Every language change should include tests covering:

* Valid syntax.
* Invalid syntax where applicable.
* The resulting syntax-tree structure.
* Relevant JSON Schema validation.
* Existing syntax that must remain compatible.

Prefer small, focused examples that isolate the changed construct.

## Generated Files

Generated parser files are release artifacts.

After modifying the grammar:

* Regenerate the parser using the project's configured Tree-sitter version.
* Do not manually edit generated parser files.
* Commit generated files when they are tracked by the repository.
* Ensure that generated artifacts correspond exactly to the grammar committed in the same version.

The Tree-sitter CLI version used to generate the parser should be kept consistent across development and releases.

## Release Checklist

Before releasing a new version:

* [ ] Version number updated.
* [ ] Grammar updated.
* [ ] Generated parser regenerated.
* [ ] Grammar tests pass.
* [ ] JSON Schema tests pass.
* [ ] Existing examples validated.
* [ ] New or changed syntax documented.
* [ ] Changelog updated.
* [ ] Generated artifacts committed.
* [ ] Working tree is clean.
* [ ] Release tag matches the project version.

## Versioning Principle

When in doubt, version according to the public interface exposed by the grammar, not merely according to the amount of code changed.

A small grammar change can require a major version bump if it changes the syntax tree or invalidates existing `ryspec` documents. Conversely, substantial implementation changes can remain a patch release if they do not change the language or its public syntax-tree interface.
