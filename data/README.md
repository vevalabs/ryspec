# Corpus

A corpus of `ryspec` documents, one directory per verdict the format has:

| directory | holds |
| --- | --- |
| `valid/` | documents the schema accepts |
| `invalid/` | documents the schema rejects, and documents that break a rule nothing implements |
| `malformed/` | documents only a parser could reject, and there is none |
| `semantic/` | schema-valid documents that each break one rule in [`SPEC.md`](../SPEC.md#what-the-schema-cannot-check)'s "What the schema cannot check" |

Every file names the schema in a `#:schema` header, so any JSON Schema
validator that reads TOML -- an editor's included -- can check it. Nothing in
this repository runs them: every file in `valid/`, `malformed/` and `semantic/`
must pass the schema, and an `invalid/` file's header decides its verdict.

**Only the schema rejects anything here.** A file declaring
`#:expect-semantic-error` or `#:expect-grammar-error` is owed a rejection by
nobody. The grammar that owed the second and the loader that owed the first
are both out of the repository. Those files and their markers are kept, because the rule
each states is still true of the format and the file is still the fixture for
it. What every one of them owes *now* is the other half of its old contract,
and it is the half that catches a schema grown too strict: it must validate.

`invalid/` therefore has no single verdict of its own. Most of its files fail
the schema and the rest stand on a rule nothing enforces. Each file's header is
what decides it.

Files nest, so a directory is free to group what belongs together.

`malformed/` stays a directory of its own rather than folding into `invalid/`.
The schema types an expression as any parenthesised string and never looks
inside one, so a malformed expression is well-formed to it -- and if one of
those files ever stops validating, the fault it carries is no longer a
parser's alone and it belongs in `invalid/` instead.

## The operator half

`operators/` under `valid/` and `invalid/` is the systematic part: one document
per temporal operator -- `prev`, `once`, `historically` and `since` looking
backward, `next`, `eventually`, `always` and `until` looking forward.

Each valid file puts its operator through every shape it accepts, **in both
spellings**: prefix form, where a rule is an operator followed by its operands,
and expression form, the infix spelling. Bare, each metric bound, over a
boolean rule, over a comparison, and over and under another operator of its own
cone -- each prefix rule has an expression twin beside it, and the file's second
property is written in expression form as well. One construct has no twin and
says so: `prev` has no infix spelling. A comparison is braced whole, as
`{speed > 30}`.

Each operator's negatives cover the arity, the cone and the bound. The arity
and bound ones fail the schema; the cone ones no longer do -- the schema settles
shape and says nothing about cones -- so they declare a semantic error and
assert only that the schema accepts them. The expression-form negatives live in
`malformed/`, an infix fault being beyond anything that reads the document now.

The files at the top level of `valid/` are the other half -- whole documents,
each showing some part of the format in a shape a real spec would take.

## How this differs from the other sets

[`examples/`](../examples/) is documentation: one file per topic the README
covers, written to be read alongside it. This corpus is written to be run --
breadth over narration, a construct per file rather than a topic.

[`semantic/`](semantic/) is one fixture per rule in
[`SPEC.md`](../SPEC.md#what-the-schema-cannot-check)'s "What the schema cannot
check", and every file there is schema-valid by construction. `invalid/` has no
such restriction: most of it is rejected by the schema, which those fixtures
never exercise.

## Adding a file

Drop a `*.toml` in `valid/` and give it a `#:schema` header, as every file
here has, and a comment saying what it is there to cover.

A negative file declares the diagnostic it expects in a second header line, and
carries exactly one fault so that diagnostic is unambiguous:

```toml
#:schema ../../schemas/v0/ryspec.schema.json
#:expect-semantic-error is greater than max
```

The layer is `schema`, `semantic` or `grammar`, and the rest of the line is a
substring of the reported error. Naming the layer is the part worth pinning: a
file meant to fail the schema that instead slips through to a semantic error is
testing something other than what it claims to.

`#:expect-grammar-error` carries no substring -- a parser reports a position
rather than a message, and a line and column are not worth freezing. A file
declaring it belongs in `malformed/` and has to pass the schema.

A `semantic` file must pass the schema too, that being the whole of what
it asserts today. Only a `schema` file is rejected by anything, and only it
names a substring to match.
