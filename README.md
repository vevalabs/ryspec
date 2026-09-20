# Reelay Specification Format

The Reelay Specification (`ryspec`) Format is a declarative specification format focused on expressing multiple temporal logic properties. It is based on the TOML specification format and is designed to provide a simple, human-readable representation of temporal logic specifications consumed by runtime verification tools and reasoning agents.

A `ryspec` document can contain any number of temporal logic properties. Each property is declared using a TOML `[[properties]]` array-of-tables entry and specifies a temporal logic formula through the `check` field.

A single property can be expressed as follows:

```toml
[[properties]]
name = "my_property"
check = "({s} -> once[3:10] {p})"
```

Multiple properties can be specified in the same document:

```toml
[[properties]]
name = "my_property"
check = "({s} -> once[3:10] {p})"

[[properties]]
name = "other_property"
check = "((once[:10] {q}) -> ((not {p}) since {q}))"
```

Each property is evaluated independently while sharing the signals referenced by its temporal logic formula.

Names inside an expression are written in braces. The braces delimit a name against the operators around it, and they mark a name rather than a kind: `{p}` resolves through one namespace to a variable, a named rule or a parameter, whichever declares it. Where a field takes a bare name instead of a parenthesised formula -- `given = "subexpr1"` below -- the name is written without braces. A metric bound follows the same rule: a number is written bare and a parameter braced, as in `once[3:10]` and `once[{min_delay}:{deadline}]`.

Expression strings are accepted by the schema wherever a rule is allowed, but the runtime cannot execute them yet: a loader accepts the syntax and reports `not yet supported`. The prefix form below is what runs today.

Expression form is also a subset of prefix form. The `prev` operator and `equiv` have no infix spelling, so a property needing either writes that rule in prefix form. The six comparisons are spelled `<`, `<=`, `>`, `>=`, `==` and `!=` between a braced name and a number or another braced name -- `({speed} > {speed_max})` is `["gt", "speed", "speed_max"]`. Neither side of a comparison is a formula, in either spelling.

Every property carries a `name`, which is unique across the file and addresses its verdict. A property may also separate its antecedent condition (`given`) from the checked condition (`check`):

```toml
[meta]
title = ""
description = ""
author = "John Doe"

[[properties]]
name = "p1"
given = "(once[:10] {q})"
check = "((not {p}) since {q})"

[[properties]]
name = "p2"
check = "({s} -> once[3:10] {p})"
```

The `given` field specifies a condition under which the property is evaluated, while `check` specifies the temporal logic condition that must hold.

## Using Prefix Form

The `ryspec` format also supports a prefix representation of temporal logic formulas. Prefix form represents an expression as an operator followed by its operands, allowing formulas to be represented without infix syntax or precedence rules.

For example:

```toml
[[properties]]
name = "p1"
given = "cond1"
check = ["since", "not_p", "q"]

[properties.rules]
cond1 = ["once", "q", { min = 3, max = 10 }]
not_p = ["not", "p"]
```

Named rules can be used as subformulas within a property. This allows complex specifications to be decomposed into smaller, reusable expressions.

## Shared Rules

Named rules can also be declared at the document level and shared by multiple properties:

```toml
[rules]
subexpr1 = "(...)"
subexpr2 = "(...)"
subexpr3 = "(...)"

[[properties]]
name = "property1"
given = "subexpr1"
check = "({subexpr2} and {subexpr3})"

[[properties]]
name = "property2"
given = "({subexpr1} and {subexpr2})"
check = "subexpr3"
```

Document-level rules provide a mechanism for defining common subformulas once and reusing them across multiple properties. This is particularly useful when a specification contains repeated temporal patterns or when properties are constructed from a common set of domain-specific conditions.

## Quantifiers

`forall` and `exists` quantify a rule over values rather than over time. Each binds one or more names and holds when the rule it quantifies holds for every binding, or for at least one:

```toml
[monitor]
inputs = ["sensor_id", "reading"]

[rules]
every_sensor_in_band = [
  "forall",
  ["implies", ["eq", "sensor_id", "s"], ["lt", "reading", 120]],
  { vars = ["s"] },
]
```

The trailing table is the binding, and it sits where a metric bound sits on a temporal operator. It is required and takes exactly one key, `vars`, holding at least one name: a quantifier binding nothing quantifies over nothing.

A quantified name ranges over the **active domain** -- the values that name's position has actually carried in the trace so far, not every value its type could hold. `every_sensor_in_band` above therefore says *of the sensor ids seen so far, each one's readings have stayed under 120*, and says nothing about a sensor that has not yet reported. Neither quantifier needs a domain declared anywhere, because the trace is the domain.

**A quantified name is local.** It is bound by the quantifier that lists it and by nothing else: it is not a variable, it is declared in no table, it takes no slot in any `[monitor]` list, and it is not an implicit input the way an undeclared name in a rule otherwise is. Three things follow, and the loader reports each:

- the name must be free where it is bound -- neither shadowing a variable, rule or property, nor rebound by a quantifier already inside one that binds it, so that every name in a rule means one thing;
- it must be read by the rule it quantifies, a quantifier that binds a name the rule never mentions being a typo more often than an intention;
- and it must be read *as a value*, on one side of a comparison. A quantified name holds a value drawn from the trace, so it is neither a rule in its own right nor something a metric bound can be sized by.

Quantification does not reach through a name. A rule referred to by name is resolved where it was declared, and the quantifier's names are not bound there, so `["forall", "some_rule", { vars = ["s"] }]` binds an `s` that `some_rule` cannot see. That is a binding nothing reads, and the second check above reports it.

A quantifier carries no time direction of its own. Like `not`, it takes the cone of the rule beneath it and may stand in either, so a quantified rule nests under `once` and over `always` alike.

Expression form spells a quantifier with its bound names in braces and a dot before the rule they range over:

```toml
[[properties]]
name = "every_sensor_in_band"
check = "(forall {s} . (({sensor_id} == {s}) -> ({reading} < 120)))"
```

The runtime cannot execute a quantifier yet, in either spelling: the schema accepts both, the checks above are made, and a loader reports `not yet supported`.

## Variables, Partitions and Sources

A variable is one value in the monitor's value space. Both tables in this section are optional -- see [Deduction](#deduction) for the minimal form, and read this one as what a file says when it wants more than the defaults.

The `[variables]` table declares variables by name. It does not say which partition a name belongs to: that is decided solely by the `[monitor]` lists below, and a name in none of them is an input.

| partition | meaning |
| --- | --- |
| input | fed in from outside at each step (the default) |
| output | computed and published -- a property's verdict, or a rule named in `[monitor].outputs`, whether file-level or private to a property |
| parameter | set before the run and held, carrying an `initial_value` |

The set is deliberately smaller than FMI 3.0's causality, which also has `local`, `independent`, `calculatedParameter` and `structuralParameter`: the runtime's value space has exactly these three partitions, in this order, and nothing else to name.

`type` is one of `bool`, `number`, `text` or `binary`, and defaults to `number`. A `text` variable is a document and a `binary` one a blob; give either a `format` and other variables can read fields out of it through a `source` path.

`unit` and `description` describe the signal for whoever feeds or reads it -- an FMI adapter writing a `modelDescription.xml`, a trace reader checking a column.

```toml
[monitor]
inputs = ["speed", "brake"]
parameters = ["speed_max"]

[variables]
speed = { type = "number", unit = "m/s", description = "vehicle speed" }
brake = { type = "bool" }
velocity = { source = "speed", type = "number", unit = "m/s" }
speed_max = { initial_value = 50.0, min = 0.0 }
```

`[monitor]` is the interface -- what the monitor reads and publishes. How it is *built* is a separate question, answered by `[monitor.runtime]`:

```toml
[monitor.runtime]
allocation_size = 65536
buffer_size = 4096
```

Both are in bytes and both are optional; absent either, the build sizes itself from the config. A file with nothing to say about sizing writes no such table.

**`[variables]` describes; `[monitor]` selects and orders.** Each of the three partitions has a list of its own -- `inputs`, `outputs` and `parameters` -- and each list's order *is* that partition's order. They are lists rather than tables because a table's key order carries no meaning, so `[variables]` cannot carry it: an author who matches a trace's column order gets a straight memcpy ingest only because `inputs` fixes it.

Because the lists alone decide the partition, `[variables]` carries no key restating it, and the two can never disagree. What a file can still get wrong is putting a variable in a list its declaration does not suit -- a `source` or a `format` on a name in `outputs` or `parameters`, an `initial_value` on a name in no list -- and the loader reports those, the schema having no way to see which list a name is in. An input is listed exactly when it has no `source`: `velocity` above has one, so it reads a slot another variable already holds rather than claiming one of its own.

Every name in `inputs` and `parameters` is a variable reference and nothing else. An `outputs` entry is looser, because it publishes a value the file already computes somewhere: it names a `[variables]` declaration, a property whose verdict is published, a file-level rule, or a rule private to a property, written `<property>.<rule>`. One namespace, so the name is all it takes to reach the first three. What a declaration adds is the interface detail -- a `type`, a `unit`, a `description` -- and a published name without one takes the defaults.

```toml
[monitor]
outputs = ["respond_bqr", "guard"]

[variables]
respond_bqr = { type = "bool", description = "RespondBQR verdict" }
guard = { type = "bool" }
```

`respond_bqr` above takes its value from the property of that name and `guard` from a file-level rule; either could have been listed without the `[variables]` entry beside it, which is there to give the published signal a type and a description. A verdict published by default -- the set is every property in declaration order, which `outputs` replaces when present -- needs no entry either way, having no list to be referenced from.

A parameter carries its start value as `initial_value`; `min` and `max` are optional and bound it for an embedding that exposes it. A number written inline in a rule is an anonymous parameter -- the loader allocates it a slot exactly as if it had been declared -- so naming one buys addressability and a place in `parameters`, nothing else. The anonymous ones follow the named, in the order the loader meets them.

## Sources

`source` says where a variable's value comes from. It is a dotted path: the first segment names another variable, and the segments after it address a field within that variable's value. `velocity` above is the degenerate one-segment case -- no field, so it reads the whole of `speed`.

The segments after the first are what let a structured input be monitored. A JSON telemetry frame is a variable like any other and takes an input slot of its own; the variables that matter to the rules are the fields read out of it:

```toml
[monitor]
inputs = ["vehicle_state"]

[variables]
vehicle_state = { type = "text", format = "json", description = "telemetry frame" }
speed = { type = "number", source = "vehicle_state.vehicle.speed" }
gear  = { type = "number", source = "vehicle_state.drive.gear" }
```

`format` says how to decode the variable a path reaches into, and is written once, on that variable -- so `speed` and `gear` above name one frame, decoded once, and neither claims an input slot. Only a `text` or `binary` variable may carry a `format`, there being nothing to decode in a number, and only an input carries a `source` or a `format`: an output is computed and a parameter is set.

A dotted path cannot address an array element, and is ambiguous where a key in the decoded document itself contains a dot. Neither is reachable in this format.

Decoding happens in the loader, never in the runtime core, which parses nothing.

## Deduction

`[monitor]` and `[variables]` are both optional, and so is every key in `[monitor]`. A file that wants none of what they offer writes neither, and the loader deduces the value space from the properties and rules alone:

```toml
[[properties]]
name = "never_both"
check = ["not", ["and", "p", "q"]]

[[properties]]
name = "q_follows_p"
given = "p"
check = ["once", "q"]
```

A name in a rule that is not a rule, a property or a declared variable can only be an input, so `p` and `q` above are allocated as inputs -- `number` by default, `bool` where their use demands it. The published set defaults to every property in declaration order.

Declaring is refinement, not permission. Write `[monitor].inputs` to pin the input partition's order, which otherwise falls out of first use, and with it the memcpy ingest a matching trace gets. Write `[variables]` to say a type, a unit, a description or a `source`. Add either one and the cross-checks above apply to what it says, but a file owes neither.

An absent list and an empty one differ, and this is the only place they do: omitting `inputs` asks the loader to deduce the partition, while `inputs = []` states that there is none. The same holds for the other three lists.

The exception is a **parameter**, which cannot be deduced: a threshold has no sensible default value. A rule that reads one needs a `[variables]` entry carrying its `initial_value`, and a name in `[monitor].parameters` that has no such entry is an error the loader reports. An inline number in a rule is the way to avoid saying so -- it becomes an anonymous parameter, with no name to declare.

## Naming and Visibility

Variables, rules and properties share one namespace, and a name means one thing in it. What may not be duplicated is a *source of value*: two rules, or a rule and a property, or a rule and an input, cannot share a name. Every name in `[monitor].inputs` and `parameters` is a plain identifier -- no dots. An `outputs` entry is the one exception: `<property>.<rule>` reaches a rule private to a property, and is the only dotted form a `[monitor]` list accepts.

A published variable is the exception that proves the rule, because it produces no value of its own. Declaring

```toml
[monitor]
outputs = ["guard"]

[variables]
guard = { type = "bool" }

[rules]
guard = ["and", "r_and_not_q", "once_q"]
```

is not two definitions of `guard`. The rule is the value; the variable is that value's declaration as part of the interface, and the shared name is the binding between them. The same holds for a published property verdict. This is how every published name in `data/` is written.

That is what makes `[properties.rules]` a visibility boundary rather than a convenience: a rule scoped to a property is private to it, and two properties may each define a `rhs` without colliding. Publishing one without moving it to file level means naming it qualified -- `guard_holds.rhs` -- in `outputs`, which is how the name stays unambiguous while two properties each keep their own `rhs`. Moving it to `[rules]`, under a name unique across the file, publishes it as a plain name instead.

A verdict's *value* is derived, never declared -- it follows from the property, and no table states it. What a file may declare is that the verdict is published, which is naming it in an `outputs` list; a `[variables]` entry beside it, as above, is optional and describes the published signal rather than defining it. The default published set is every property in declaration order, and `outputs` REPLACES that set when present.

A name also carries its rule's **time cone**. Every rule sits in one: `prev`, `once`, `historically` and `since` look backward, `next`, `eventually`, `always` and `until` look forward, and a rule holding both is rejected -- by the schema where both are written out, as in `["always", ["once", "p"]]`, and by the loader where a name hides one. `check = ["always", "looks_back"]` is a future-time rule over an identifier, and if `looks_back` is `["once", "p"]` the two cones meet just the same. The resolution follows names as far as they go, through `[rules]`, through a property's private table, and through a property named for its verdict, which carries the cone of that property's `check`.

A rule built only from names, comparisons, quantifiers and the boolean operators has no direction of its own. It reads in either cone, which is what lets one shared rule serve a past-time property and a future-time one. Expression form is where this stops: an expression is an opaque string to the loader, so the cones inside one are the parser's to judge and are not resolved here.

The same walk answers one more question. A rule defined in terms of itself -- directly, or around a ring of names -- never reaches a value, and the loader reports it where the ring closes.

## Document Structure

Nine top-level keys, of which `version` and `properties` are required:

| key | holds |
| --- | --- |
| `version` | **required.** The ryspec schema version this document targets -- `0`, today |
| `properties` | **required**, and may be empty. The properties, each yielding one verdict |
| `rules` | file-level named subformulas, shared across properties |
| `variables` | the value space, keyed by name |
| `monitor` | the interface: the three partition lists, plus `[monitor.runtime]` sizing |
| `features` | format flags -- currently none active; `disable_expressions` is reserved |
| `namespace` | a dotted name identifying this file's names; an identity, never resolved |
| `meta` | free-form document metadata |
| `extras` | free-form data outside the document's metadata |

The document root is closed: anything else is rejected, which is what catches a
misspelled or retired table name. `meta` and `extras` are the deliberate
exceptions -- each takes arbitrary keys, `meta` beyond the `title`, `author`,
`license`, `description` and `url` it names. `meta` is the place for facts
about the document; `extras` is the place for anything else the format has
no opinion about. `meta.url` is the document's own canonical location --
where the authoritative copy of this file lives, not necessarily fetched.

[`DESIGN.md`](DESIGN.md) records why the format is shaped this way -- one
namespace, three partitions, two spellings -- and what each decision rules out.

## What the schema cannot check

JSON Schema validates shape, not resolution, so the following are well-formed to
the validator and are the loader's to reject. A `[variables]` entry says nothing
about which partition it is in, so everything that turns on a partition is here:

- a name in two `[monitor]` lists at once
- two `[[properties]]` entries sharing a `name`
- an `inputs` or `parameters` entry with no `[variables]` declaration
- an `outputs` entry that names no variable, property or rule -- including a
  qualified `<property>.<rule>` whose property or private rule does not exist,
  and one carrying more than the single dot the qualified form takes
- a `source` or a `format` on a name in `outputs` or `parameters` -- only an
  input carries either
- an `initial_value` on a name in no `[monitor]` list -- only a parameter has one
- a listed parameter whose declaration carries no `initial_value`
- `min` greater than `max`, on a parameter or on a metric bound
- a `source` path whose head is undeclared, or declares no `format`
- a rule naming a `text` or `binary` variable -- there is no value there to compare
- two sources of value sharing one name
- a quantifier variable shadowing a declared name, or rebound by a quantifier
  nested inside the one that binds it
- a quantifier variable the quantified rule never reads
- a quantifier variable read as a rule or as a metric bound rather than as a
  value on one side of a comparison
- a rule mixing the two time cones behind a name -- a past-time rule read
  inside a future-time one, or the two combined
- a rule defined in terms of itself, directly or around a ring of names

## Check with schema

The validator is a Python package under [`python/`](python/) -- source at
`python/src/ryspec`, its test suite at `python/tests` -- packaged from the
`pyproject.toml` at the root, so `pip install git+<repo-url>` installs it and
provides a `ryspec` command. `ryspec validate` checks every `*.toml` under a directory
against the bundled schema:

```sh
ryspec validate data/valid
```

It exits non-zero on the first file that fails, reporting the JSON Pointer to the
offending value. Beyond schema validation, `ryspec validate` also performs the
checks listed under "What the schema cannot check" above, reporting whichever
of the two fails first. `--expect-invalid` inverts the assertion, for a
directory of negative fixtures. Editors read the `#:schema` header at the top
of each file for the same checks inline.

Only the first of those two is Python's. The schema layer is `jsonschema`
reading `schemas/v0/ryspec.schema.json`; the loader layer is the C library
below, which `ryspec.semantics` binds with `ctypes` and over which the package
holds no rules of its own. So `ryspec validate` and `ryspec-db` reach one
verdict because they are one implementation, and there is no second copy of
the checks to drift.

That makes the package a compiled one, and it is compiled by this repository's
own build system: `pip install .` runs `CMakeLists.txt` through
[py-build-cmake][], which puts `libryspec` and the schema inside the package.
So there is one description of how the C is built rather than two that could
drift, and an installed `ryspec` needs no CMake afterwards and no runtime on
the host -- though installing it needs a C compiler, CMake, and the network for
the runtime the build fetches. In a checkout the package finds the library
CMake builds into `build/` instead, so `pytest` works after `make` and needs no
`pip install`. `$RYSPEC_LIBRARY` names a library explicitly and overrides both.

[py-build-cmake]: https://tttapa.github.io/py-build-cmake/

See [`examples/`](examples/) for a working `*.toml` file per topic covered above --
`ryspec validate examples` checks them all against the schema.

[`data/`](data/) holds the corpus the test suite runs, one directory per
verdict: `data/valid/` is what every layer accepts, `data/invalid/` is what the
schema or the loader rejects, and `data/malformed/` is what only the grammar
rejects. Each negative declares the diagnostic it expects in a header comment.
The first two are checked in opposite directions:

```sh
ryspec validate data/valid
ryspec validate data/invalid --expect-invalid
```

`data/malformed/` passes `ryspec validate` and is meant to -- an expression is
an opaque string to the schema and the loader alike, so a malformed one is well
formed to both and only the parser sees it. Under `operators/` on each side sits
a document per temporal operator, written in prefix form and expression form
both; see [`data/README.md`](data/README.md).

## Parse with tree-sitter

The repository root is the parser: a [tree-sitter][] grammar and the C parser
generated from it. It reads a document one level finer than the schema does:
`[monitor]`, `[variables]`, `[rules]` and `[[properties]]` parse into nodes of
their own, a prefix rule parses into an operator with its operands, and a rule
in expression form parses into an expression tree rather than a string. That is
what an editor colours, what a language server queries, and what a tool walks
when it wants the structure of a file rather than its value. `ryspec` is TOML,
so this is a TOML grammar with the format's own structure named on top of it,
and everywhere the format has no opinion -- `[extras]` above all -- the generic
TOML rules apply.

| path | holds |
| --- | --- |
| [`src/grammar.json`](src/grammar.json) | **the grammar.** Authored by hand, and what `tree-sitter generate` reads and the CLI loads. No `grammar.js`, and so no Node |
| `src/parser.c`, `src/node-types.json` | generated; do not edit |
| `src/scanner.c` | the external scanner: line endings and multi-line string delimiters |
| [`cli/ryspec-parse.c`](cli/ryspec-parse.c) | the application over the parser |
| [`test/corpus/`](test/corpus/) | `tree-sitter test` cases |
| [`queries/highlights.scm`](queries/highlights.scm) | syntax highlighting |

The build is [CMake][], with a `Makefile` over it for the commands people type:

```sh
make             # configure and build build/libryspec.so and build/ryspec-parse
make generate    # src/grammar.json -> src/parser.c
make test        # the corpus under test/, the corpus under data/, and the Python suite
make wheel       # the Python package into dist/
make sdist       # the source distribution into dist/
make install     # PREFIX=/usr/local by default
```

which is the same as driving CMake directly:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --build build -t generate
ctest --test-dir build
cmake --install build
```

Nothing has to be generated to build: `src/parser.c` is committed beside the
grammar, so a C compiler and the runtime fetched below are the whole of it. The
`generate` target and the `grammar-corpus` test are the two things that need
the [tree-sitter CLI][], and both drop out of the build when it is absent.

A document that parses is well-formed; whether its names resolve is still the
loader's question, and none of it -- whether two properties share a name,
whether a parameter has an `initial_value` -- is visible to a context-free
grammar. So the parser accepts every fixture under
[`python/tests/fixtures/invalid/`](python/tests/fixtures/invalid/) that `ryspec
validate` rejects, and every `data/invalid/` document rejected for a semantic
reason rather than a schema one. The other direction is
[`data/malformed/`](data/malformed/), where the parser is the only layer that
rejects anything: a document may satisfy the schema and still hold an
expression that is not one.

### `ryspec-parse`

[`cli/ryspec-parse.c`](cli/ryspec-parse.c) is what runs the parser: `ryspec
validate` one layer down, answering the question neither the schema nor the
loader can reach.

```sh
build/ryspec-parse data/valid                         # every file must parse
build/ryspec-parse --expect-malformed data/malformed  # every file must not
build/ryspec-parse --corpus data                      # each file's header decides
build/ryspec-parse --print-tree examples/quantifiers.toml   # the tree, as an s-expression
```

A path is a file or a directory to recurse into, and a failure is reported with
the position of the first ERROR or missing node. `--corpus` is the paragraph
above made runnable: a file declaring `#:expect-grammar-error` owes the parser a
rejection, one declaring `#:expect-semantic-error` owes it a clean parse, and
one declaring `#:expect-schema-error` owes it neither, a bad operator being
something the grammar may reject or may leave to the schema. So the whole corpus
goes through in one command, and `data-valid`, `data-malformed` and
`data-corpus` under `ctest` are that command and its two halves.

Generated code builds against nothing, but *running* a parser needs the
tree-sitter runtime, and so does the database below. CMake fetches it with
`FetchContent`, at the tag and checksum two `set()` lines in
[`CMakeLists.txt`](CMakeLists.txt) pin -- a version that speaks the language
ABI `src/parser.c` is generated at -- and compiles its own `lib.c`
amalgamation into a static library, which
goes inside `libryspec`. So the shipped library carries the runtime rather than
linking against one, and `make install` installs this project and no more:
nothing of the runtime's own build or install rules is part of this build,
because only its sources are read.

A runtime already installed on the host is used in preference, so a
distribution packaging `ryspec` against its own copy gets what it expects.

```sh
cmake -S . -B build -DRYSPEC_FETCH_TREE_SITTER=ON    # fetch it regardless
cmake -S . -B build -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
      -DFETCHCONTENT_SOURCE_DIR_TREE-SITTER=/path/to/tree-sitter    # or no network at all
```

The Python package fetches the same runtime at the same pin, because building
it *is* running this file -- so `make` and `pip install .` compile identical
source, and the second line above is how a package build with no network gets
one:

```sh
pip install . -C override=cmake.options.FETCHCONTENT_FULLY_DISCONNECTED=ON \
              -C override=cmake.options.FETCHCONTENT_SOURCE_DIR_TREE-SITTER=/path/to/tree-sitter
```

## The database

A parse tree is a shape. What a document *means* -- which names it declares,
what rule each one stands for, and whether the two hold together -- is a second
reading, and [`src/database.h`](src/database.h) is where it lives. It is part
of `libryspec` wherever a tree-sitter runtime is present, so a C consumer can
ask what a name means without going through Python.

```c
ryspec_database *db = ryspec_database_new();
ryspec_database_load(db, "data/valid/published_values.toml");
ryspec_database_check(db);

ryspec_symbol_id id = ryspec_lookup(db, ryspec_intern(db, "guard", 5));
const ryspec_symbol *guard = ryspec_symbol_at(db, id);   // roles, type, rule
```

Two things shape it.

**One name space, under the document's namespace.** Variables, rules and
properties share one namespace, so the database holds them in one table, keyed
by `<namespace>.<name>` -- and a rule private to a property by
`<namespace>.<property>.<rule>`, which is the qualified `outputs` form with the
namespace in front. That key is the whole of the visibility rule: two
properties may each declare an `rhs` because their keys differ, and
`guard_holds.rhs` in `outputs` reaches one of them in a single lookup. It is
also what lets one database hold many documents at once. `namespace` has been
inert until now -- an identity the format declares and nothing reads. This is
what reads it.

A name is one record however many times it is written, and the roles it plays
are a bitset on that record. That is why `guard` in `[rules]` and `guard` in
`[variables]` are not two definitions of anything -- one symbol, two roles --
while a rule and an input of one name are two sources of value and an error.

**Rules are deduplicated, and so are strings.** A rule is interned: a
structurally identical rule written in five places is one term, and both
spellings lower into the same representation, so `["not", "p"]` and `"(not
{p})"` are the same rule. Names are resolved to their qualified form before
interning, so `["once", "rhs"]` in two properties correctly stays two rules.
Every string the database keeps is interned too.

```sh
build/ryspec-db data/valid                    # every file must check out
build/ryspec-db --expect-invalid data/invalid # every file must report
build/ryspec-db --corpus data                 # each file's header decides
build/ryspec-db --together --stats data/valid # what a whole corpus shares
```

A file at a time by default, each in a database of its own, because that is
what a document means on its own terms. `--together` puts every file named into
one database instead, which is the mode the namespace exists for: two
documents' names meet under theirs, a rule both state is stored once, and two
documents sharing a namespace are checked against each other. Pointed at
documents that declare no namespace -- most of `data/` -- `--together` reads
them as one document, which by the format's rules they are, and the collisions
it reports are real.

`--dump-symbols`, `--dump-rules` and `--dump-strings` print the three tables;
`--stats` prints what the sharing comes to. On
[`data/valid/operators/once.toml`](data/valid/operators/once.toml), which
writes nine rules twice over -- prefix form and expression form, under eighteen
names -- the dump shows `plain` and `expr_plain` resolving to the same term,
and seven more pairs behind them.

The database also carries the checks of "What the schema cannot check" above.
They were written in Python first and are now here alone:
[`python/src/ryspec/semantics.py`](python/src/ryspec/semantics.py) binds this
library rather than repeating it, so `ryspec validate` and `ryspec-db` reach
one verdict because they run one implementation. `db-valid`, `db-semantic`,
`db-fixtures` and `db-namespaces` under `ctest` hold it to the corpus, and the
Python suite holds it to the same fixtures from the other side.

Moving the checks down a layer changed one verdict, and deliberately. The
Python loader treated a rule in expression form as an opaque string and did not
look inside one, so a cone mixed inside an expression --
`"({looks_back} and {looks_ahead})"`, where those two names resolve to a
past-time and a future-time rule -- passed it. This one lowers both spellings
into the same rules, so it resolves those names and reports what the README
forbids anywhere else. Every document in `data/` and `examples/` is accepted
either way; `test_a_cone_mixed_inside_an_expression_is_reported` is where the
change is recorded.

Because `[extras]` takes arbitrary keys, the TOML underneath has to be the
whole of TOML. [`python/tests/test_toml_conformance.py`](python/tests/test_toml_conformance.py)
re-roots every file of the standard [toml-test][] suite under `[extras]` and
parses it:

- all 208 valid TOML 1.0.0 documents parse clean, and each one decodes to the
  same value re-rooted as it does standalone;
- 432 of the 501 invalid ones are rejected. The other 69 are listed, with the
  reason, in `python/tests/toml-test-semantic.txt`: a key or table defined
  twice, or a date that is well-formed but not on the calendar. Both are
  questions about meaning rather than shape, and neither is answerable here.

Run [`scripts/fetch-toml-test.sh`](scripts/fetch-toml-test.sh) once to fetch
the suite; the conformance test skips without it.

`src/scanner.c` is adapted from [tree-sitter-toml][] (MIT).

[CMake]: https://cmake.org/
[tree-sitter]: https://tree-sitter.github.io/tree-sitter/
[tree-sitter CLI]: https://github.com/tree-sitter/tree-sitter/tree/master/cli
[tree-sitter-toml]: https://github.com/tree-sitter-grammars/tree-sitter-toml
[toml-test]: https://github.com/toml-lang/toml-test
