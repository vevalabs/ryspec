# Corpus

A corpus of `ryspec` documents, one directory per verdict:

| directory | holds |
| --- | --- |
| `valid/` | documents every layer accepts |
| `invalid/` | documents the schema or the loader rejects |
| `malformed/` | documents only the grammar rejects |

```sh
ryspec validate data/valid
ryspec validate data/invalid --expect-invalid
ryspec validate data/malformed      # passes, and that is the point

build/ryspec-parse --corpus data    # the grammar's half, all three at once
build/ryspec-db --corpus data       # the loader's half, the same way
```

All three are driven by [`python/tests/test_data_corpus.py`](../python/tests/test_data_corpus.py),
which also runs the tree-sitter parser over `valid/` and `malformed/`. Files
nest, so a directory is free to group what belongs together.

[`cli/ryspec-parse.c`](../cli/ryspec-parse.c) runs the grammar's half of the
same contract from C: `--corpus` reads each file's `#:expect-<layer>-error`
header and holds the parser to what that layer owes it. It is `ctest`'s
`data-corpus`, and it needs no Python.

[`cli/ryspec-db.c`](../cli/ryspec-db.c) runs the loader's half the same way,
as `ctest`'s `db-semantic`. It reads only `#:expect-semantic-error`: a file
declaring one owes the database a diagnostic carrying that substring, and a
file declaring a schema or grammar error owes it nothing either way, those
being other layers' to reject. It is the same check `ryspec validate` makes,
because both reach the same library.

`malformed/` is separate because `ryspec validate` cannot reject those files.
The schema types an expression as any parenthesised string and the loader never
looks inside one, so a malformed expression is well-formed to both and only the
parser sees it. Keeping those documents out of `invalid/` is what lets
`ryspec validate data/invalid --expect-invalid` stay an honest assertion.

## The operator half

`operators/` under `valid/` and `invalid/` is the systematic part: one document
per temporal operator -- `prev`, `once`, `historically` and `since` looking
backward, `next`, `eventually`, `always` and `until` looking forward.

Each valid file puts its operator through every shape it accepts, **in both
spellings**: prefix form, where a rule is an operator followed by its operands,
and expression form, the infix spelling. Bare, each metric bound, over a
boolean rule, over a comparison, and over and under another operator of its own
cone -- each prefix rule has an expression twin beside it, and the file's second
property is written in expression form as well. Two constructs have no twin and
say so: a comparison has no infix spelling, and neither does `prev`.

Each operator's negatives cover the arity, the cone and the bound, and the
expression-form ones live in `malformed/`, that being the layer an infix fault
reaches.

The files at the top level of `valid/` are the other half -- whole documents,
each showing some part of the format in a shape a real spec would take.

## How this differs from the other two sets

[`examples/`](../examples/) is documentation: one file per topic the README
covers, written to be read alongside it. This corpus is written to be run --
breadth over narration, a construct per file rather than a topic.

[`python/tests/fixtures/invalid/`](../python/tests/fixtures/invalid/) is one fixture per rule
in the README's "What the schema cannot check", and every file there is
schema-valid by construction, because its job is to pin a loader diagnostic.
`invalid/` here has no such restriction: most of it is rejected by the schema,
which those fixtures never exercise.

## Adding a file

Drop a `*.toml` in `valid/` and it is validated and parsed; nothing else to
edit. Give it a `#:schema` header, as every file here has, and a comment saying
what it is there to cover.

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

`#:expect-grammar-error` carries no substring -- the parser reports a position
rather than a message, and a line and column are not worth freezing. A file
declaring it belongs in `malformed/`, and has to pass `ryspec validate`: if one
ever stops passing, the fault it carries is no longer the grammar's alone and
the file belongs in `invalid/` instead.

A `semantic` file must also parse -- semantic invalidity is beyond the grammar,
which sees only syntax. A `schema` file carries no such obligation: a bad
operator or a bad rule arity is exactly what the grammar is entitled to reject.
