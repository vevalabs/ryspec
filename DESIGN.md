# Design

### A subset of TOML

The format is TOML so that a specification is readable, diffable and editable
with tools people already have, and so that `[extras]` can hold arbitrary data
without inventing a syntax for it. The cost is that the grammar has to be a
whole TOML grammar before it is a ryspec one, which is why
`python/tests/test_toml_conformance.py` runs the standard `toml-test` suite
re-rooted under `[extras]`.

Two things are deliberately not supported: dotted names for individual tables,
and TOML's datetime types, which parse as plain strings. Neither earns its
complexity in a specification format.

### One namespace for symbols

Variables, rules and properties share one namespace, and a name means one thing
in it. The alternative -- a namespace per kind -- would let `guard` be a
variable and a different `guard` be a rule, and every rule that read `guard`
would then have to say which it meant.

What may not be duplicated is a **source of value**: two rules, or a rule and a
property, or a rule and an input, cannot share a name. A `[variables]`
declaration is not a source of value, which is the whole of the published-name
exception -- naming `guard` in both `[rules]` and `[variables]` is one value and
its interface description, not two definitions.

`[properties.rules]` is a visibility boundary rather than a convenience: two
properties may each define `rhs` without colliding. Publishing one means naming
it `<property>.<rule>` in `outputs`, the only dotted form a `[monitor]` list
accepts.

### Declaring describes; the monitor selects

`[variables]` says what a value *is* -- its type, unit, description, source.
`[monitor]` says which partition it is in and in what order. A `[variables]`
entry states no partition, which is why everything that turns on a partition is
the loader's to check.

An absent list and an empty one differ, and this is the only place in the format
where they do. Omitting `parameters` asks the loader to deduce the partition;
`parameters = []` states that there is none. The database records this as
`ryspec_document.monitor_lists`, because no symbol can: an empty list declares
no name.

### Deduction, and the one thing that cannot be deduced

A file owes neither `[monitor]` nor `[variables]`. A name in a rule that is not
a rule, a property or a declared variable can only be an input, so it becomes
one. Declaring is refinement, not permission.

A **parameter** is the exception, because a threshold has no sensible default:
a name in `[monitor].parameters` needs a `[variables]` entry carrying its
`initial_value`. An inline number in a rule is the way to avoid saying so --
it becomes an anonymous parameter, with no name to declare.

### Time cones

Every rule looks backward or forward, and a rule holding both is rejected. The
schema settles the case written out in full, by mirroring every rule definition
into a `pastRule` and a `futureRule`; a name hides the other case, and the
loader resolves it by following names as far as they go.

A rule built only from names, comparisons, quantifiers and the boolean
operators has no direction of its own. Neutrality is what lets one shared rule
serve a past-time property and a future-time one, and it is why the cone of a
boolean combination is its operands' where they agree rather than an error where
one is absent.

`given` gates a property's verdict rather than computing it, so it lends the
verdict no cone. The same walk that resolves cones finds a rule defined in terms
of itself, because both are the same question about a ring of names.

### Quantifiers

`forall` and `exists` quantify over the **active domain** -- the values a
position has actually carried in the trace -- rather than over a declared type.
The trace is the domain, so nothing has to be declared anywhere, and a
quantifier says nothing about a value that has not yet appeared.

A quantified name is local: bound by the quantifier that lists it and by nothing
else. It takes no slot, shadows nothing, and stands only where a value stands --
on one side of a comparison. Quantification does not reach through a name: a
rule referenced inside the operand is resolved where it was declared, where the
variable is not bound.

## The grammar

The grammar is authored by hand as `src/grammar.json`, not as `grammar.js`, so
that generating the parser needs no Node. The generated `src/parser.c` is
committed, so building needs a C compiler and nothing else.

**Every ryspec table is its own node type.** `[[properties]]` parses as
`property`, `[properties.rules]` as `property_rules_table`, `[variables.x]` as
`variable_table`. A consumer dispatches on node type rather than matching header
text, and everywhere the format has no opinion -- `[extras]` above all -- the
generic TOML rules apply.

The syntax tree is public interface. Node names, fields and child ordering are
what an editor colours and a tool walks, so changing the tree can be a breaking
change even when every document stays valid. That is what `VERSIONING.md`
versions.

Two spellings exist for every rule. **Prefix form** -- an operator followed by
its operands -- is what runs; **expression form** is the infix spelling, and it
is a subset, because `prev` and `equiv` have no infix spelling. The grammar
parses both into trees. The schema types an expression as an opaque
parenthesised string, which is what puts malformed expressions in
`data/malformed/`.

## The database

`src/database.h` is what a document *means*, read out of its parse tree: one
namespaced name space and one deduplicated rule table, with the loader's checks
on top. It exists so a C consumer -- a runtime verification engine, a language
server, a code generator -- can ask what a name means without re-deriving the
structure and without going through Python.

### What is deduplicated, and what is not

One test sorts every record:

> **A record that carries a position is an entity and is never combined by
> content. A record that carries no position is a value and is always
> deduplicated.**

| | tables | addressed by | identity |
| --- | --- | --- | --- |
| interned | strings, rules | content | equal handle means equal content |
| merged | symbols | qualified name | one record per name, ever |
| appended | bindings, documents, diagnostics | insertion order | one record per site |

That is why `ryspec_rule` holds no `TSPoint`. The moment a struct must answer
*where did this come from*, it cannot be shared by five call sites.

The test cuts the other way too, and did. An occurrence table -- one record per
rule position, holding the rule, its owner and its point -- was written and
then removed, because every one of those records is derivable: a rule position
*is* a symbol's `rule`, `given`, `check` or `impose`, and the symbol already
carries the document and the point. A table that restates another table is
worth less than the ABI it freezes.

The test is why a quantifier's bound names are a `ryspec_binding` rather than
part of the term that binds them. They carry a position and a spelling, so
putting them in the term would either break alpha-equivalence or leave a field
outside the hash key, and both are worse. The term keeps de Bruijn indices and a
count; the binding keeps the names, which is what the diagnostics have to
quote. That is the difference from the occurrence table: a binding's names are
recoverable from nowhere else.

### One key for three scopes

A symbol's key is its qualified name: `<namespace>.<name>` for a file-level
name, and `<namespace>.<property>.<rule>` for a rule private to a property.
Three consequences follow from the key alone, with no scope stack anywhere:

- two properties may each declare an `rhs`, because their keys differ;
- `guard_holds.rhs` in `outputs` is one lookup, being the private rule's key
  with the namespace in front;
- one database holds many documents, and two that share a namespace are checked
  against each other.

`namespace` was inert before this -- an identity the format declared and nothing
read. This is what reads it.

### Roles, not records

A name is one record however many times it is written, and the roles it plays
are a bitset on that record. Merging is what makes the published-name case fall
out: `guard` in `[rules]` and `guard` in `[variables]` is one symbol with two
roles. A collision is then a symbol carrying two roles that both produce a
value, which is nearly the whole of that check.

### Interned rules

Rules are hash-consed. A structurally identical rule written in five places is
one term, and **both spellings lower into the same representation**, so
`["not", "p"]` and `"(not {p})"` are one rule. Four canonicalisations decide how
much sharing there is, and each is a choice:

- `->` and `implies` are one operator;
- a prefix `{min, max}` and an infix `[m:n]` produce one bound;
- `and`, `or` and `xor` are flattened, so left-associative infix meets n-ary
  prefix; `implies` and `equiv` are not, not being associative;
- a bound name becomes a relative de Bruijn index, so alpha-equivalent
  quantifiers share a term.

Names are resolved to their qualified form *before* interning, so `["once",
"rhs"]` in two properties correctly stays two rules.

Hash-consing pays for itself twice over in the checks: the cone memo is an array
indexed by rule id rather than a map keyed by a position, and the term graph is
acyclic by construction -- a reference holds a name, not a rule -- so the only
cycle to find is the one a chain of names closes.

### Interned strings

Every string the database keeps is interned, decoded first, so the two TOML
spellings of one value land on one entry. Three things follow: every comparison
is an integer compare; a corpus costs one copy of `"bool"`, `"m/s"` and each
repeated description; and a qualified name is a handle, so the namespace prefix
costs nothing to compare.

### What the database does not hold

`[meta]` and `[extras]` are skipped. They are the format's two open tables --
arbitrary keys, arbitrary nesting, facts *about* a document rather than part of
its value space -- and no name resolves through either. A consumer that wants
them has the parse tree.

## One artifact

The database is part of `libryspec` rather than a library beside it, so there is
one thing to build, link and install: a shared object exporting
`tree_sitter_ryspec` and the `ryspec_*` interface of `src/database.h`. The
library exports exactly what that header declares; everything `src/internal.h`
shares between the three sources is hidden, so the ABI is the documented one.

## The Python package is a binding

`ryspec validate` runs two of the three layers, and only one of them is
Python's:

- the **schema** layer is `jsonschema` reading the bundled schema. That is
  declarative configuration applied by a third-party validator, not an
  implementation of anything.
- the **loader** layer is the C library, bound with `ctypes` in
  `ryspec.semantics`. The package holds none of those rules.

The binding takes seven functions and mirrors no struct, so adding a field to
`ryspec_symbol` costs it nothing; changing one of those seven signatures is a
break that moves the binding in the same commit.

The checks were written in Python first, and moving them down a layer changed
one verdict. The Python loader treated an expression as an opaque string and did
not look inside one, so a cone mixed *inside* an expression passed it. The
database lowers both spellings into the same rules, resolves the names, and
reports it. Every document in `data/` and `examples/` is accepted either way;
`test_a_cone_mixed_inside_an_expression_is_reported` is where the change is
recorded rather than discovered.

The diagnostic wording is the contract between the two implementations, pinned
from both sides: `EXPECTED` in `python/tests/test_semantics.py`, and the
`#:expect-semantic-error` headers under `data/`.

## The vendored runtime

The tree-sitter runtime is vendored at `vendor/tree-sitter`, not fetched.

The Python package compiles it. A wheel is built from an sdist, an sdist cannot
download anything, and walking a parse tree needs a runtime -- so the runtime
has to be in the tree. CMake compiles the same copy, which is the point: `make`
and `pip install .` build identical source, and neither needs the network or a
host package. A runtime already installed on the host is used in preference, so
a distribution packaging `ryspec` against its own copy gets what it expects.

The cost is ~800K of MIT-licensed C that has to be refreshed deliberately, which
`scripts/vendor-tree-sitter.sh` does. The version is recorded once, in
`vendor/tree-sitter/VERSION`, and CMake reads it from there so there is nothing
to keep in step.

## Known tensions

Recorded rather than fixed, because each is a decision someone should make on
purpose.

**`[monitor]` optionality.** README.md says every key in `[monitor]` is
optional; the schema requires `inputs` and `outputs` whenever `[monitor]` is
present. A file with `[monitor]` and no `outputs` -- which the README's own
quantifier example writes -- is rejected by the schema and accepted by the
loader. One of the two is wrong.

**`impose` is checked in C and not in Python.** `semantics.py` never walked it,
so an `impose` rule got no checking at all. The database walks it. AGENTS.md
says `impose` is future-only and `given`/`check` past-only, which nothing
enforces at any layer.

**Two constructs parse and do not run.** A loader reports `not yet supported`
for a rule in expression form and for a quantifier in either spelling: the
schema accepts both, the grammar parses both, and the checks are made on both.
The database lowers both too, so this layer is ahead of the runtime and will
stay so until the runtime catches up.

**`features.disable_expressions` is reserved and inert**, as is `impose` in the
schema's own description. Both are shape without meaning until something
implements them.
