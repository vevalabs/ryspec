# ryspec Specification

This is the definition of the `ryspec` language, format version `0`. It covers
how a document is laid out, the two spellings of a rule and what a rule means,
plus the rules a conforming document obeys that the JSON Schema cannot express.

Where this text and [`schemas/v0/ryspec.schema.json`](schemas/v0/ryspec.schema.json)
disagree on shape, the schema is right and this text has a bug. On everything
the schema cannot see -- meaning, resolution, expression syntax -- this text is
the authority. [Open questions](#open-questions) lists what neither settles yet.

## Conformance

A document is held to four layers, each of which presumes the one before it:

| layer | what it settles | implemented by |
| --- | --- | --- |
| TOML | the document is TOML | a TOML 1.1 parser |
| schema | which tables and keys exist, the type of each value, the spelling and arity of a prefix rule | the JSON Schema in [`schemas/v0/`](schemas/v0/) |
| grammar | the syntax inside an expression-form string | nothing, today |
| semantic | what a name refers to, types across references, time cones | nothing, today |

A document **conforms** when it passes all four. A JSON Schema validator checks
the first two, so a document it accepts may still hold an expression the
[grammar](#rules-in-expression-form) does not derive, or break a rule in
[What the schema cannot check](#what-the-schema-cannot-check). The corpus
fixture for each such rule is cited beside it, and it carries an
`#:expect-grammar-error` or `#:expect-semantic-error` header that nothing yet
answers to.

## Document structure

A document is a TOML table. Its root holds these keys and no others:

| key | holds |
| --- | --- |
| `version` | **required.** The format version, the string `"0"` |
| `meta` | `title` and `description` of the document, and nothing else |
| `variables` | the value space: one declaration per variable, keyed by name |
| `monitors` | the runtime interfaces over the document's properties, keyed by name |
| `rules` | rules of the anonymous namespace, keyed by name |
| `properties` | properties of the anonymous namespace, keyed by name |
| `namespace` | the named namespaces |
| `extras` | free-form data the format does not interpret |

A **namespace** groups rules and properties. The root's `rules` and
`properties` are the anonymous namespace. A named one is a path under
`namespace`, and holds `rules` and `properties` of its own:
`[namespace.com.example.braking.properties.p]` is the property `p` of the
namespace `com.example.braking`. Every header thus says what it holds: the
segments between `namespace.` and `.rules` or `.properties` are the namespace.

A namespace may also hold `extras`, free-form data about it that the format
does not interpret (`data/valid/namespace_extras.toml`). Logic sits in the
leaves: a namespace holding `rules`, `properties` or `extras` is a **leaf** and
holds no nested namespace, and any other is **intermediate** and holds nested
namespaces alone (`data/invalid/namespace_with_logic_and_nested_namespace.toml`).
So `plant.shared.arm_idle` names a rule of the leaf `plant.shared`, and
`plant` declares nothing. The root is the one exception, being the anonymous
namespace and the parent of the named ones at once.

No namespace is named `rules`, `properties` or `extras`, since a key with one
of those names makes its namespace a leaf. A header that means a nested
`extras` is read as free-form data and refused nowhere, which is the price of
the name. Everything
a namespace holds is a table: a rule written straight into one is refused
(`data/invalid/namespace_holds_rule_directly.toml`), and so is a misspelled
`properties` (`data/invalid/namespace_properties_misspelled.toml`).
`namespace` itself is no namespace: `[namespace.properties]` would be the
anonymous namespace under a second header, and is refused
(`data/invalid/namespace_root_holds_properties.toml`), as is
`[namespace.extras]`, the document's being `[extras]`
(`data/invalid/namespace_root_holds_extras.toml`).

A rule and a property differ in what a monitor offers rather than in what they
mean: a rule is a helper that no monitor publishes, and a property is a
requirement, published and described.

```toml
[meta]
title = "Two cells"

[rules]
interlock = ["not", "arm_moving"]

[properties.arm_safe]
check = ["historically", "interlock"]

[namespace.south.rules]
interlock = ["not", ["and", "door_open", "arm_moving"]]

[namespace.south.properties.door_interlock]
check = ["historically", "interlock"]
```

An identifier -- every key, every name -- is a letter or underscore followed by
letters, digits or underscores. A **dotted path** is identifiers joined by dots.

## Names

Rules and properties live in namespaces. Variables do not: `[variables]` is the
document's one value space, and every rule in every namespace names a variable
by the bare name it is declared under.

A name written in a rule of namespace `N` resolves as follows:

1. A **bare name** is looked up where it is written: the `where` table of the
   enclosing property, `N`'s `rules`, `N`'s `properties`, and `[variables]`.
   Across those four it must find exactly one thing (Rule 11). Neither a parent
   nor a nested namespace is searched.
2. A **dotted path** is absolute, from the root: `plant.shared.arm_idle` is the
   rule or property `arm_idle` of the namespace `plant.shared`, wherever it is
   written from. A reference is its table path less `namespace.` and the
   `rules` or `properties` segment, so the rule declared under
   `[namespace.plant.shared.rules]` is `plant.shared.arm_idle`. No path reaches
   into a `where` table.
3. A bare name nothing declares is a deduced input (see [Monitors](#monitors)).

A name resolves to a variable, a rule or a property, and never to a namespace
(Rule 22).

Nothing shadows anything. A property's `where` table is a **boundary**, not a
shadow: its rules are visible inside that property alone, so two properties may
each have a private `rhs` (`data/valid/private_rules.toml`), but a private rule
may not reuse a name its namespace already gives a rule or a property.

A name resolving to a **property** stands for that property's verdict
([Verdicts](#verdicts)).

## Variables

`[variables]` declares variables by name. `type` picks which other keys a
declaration may carry:

| `type` | value | may also carry |
| --- | --- | --- |
| `bool` (the default) | a truth value | `initial_value` (boolean) |
| `number` | a number | `initial_value`, `min`, `max` |
| `text` | text, structured or not | `format = "json"` |
| `binary` | bytes | `format = "flatbuffers"` |

Every declaration may carry `title`, `description`, `source` and `extras`,
free-form data about the variable that the format does not interpret. A declaration
with no `type` is a `bool` (`data/valid/untyped_variable_is_bool.toml`).

**`source`** says where a variable's value comes from, as a dotted path. Its
first segment names another variable -- the head -- and the segments after it
address a field inside the head's value. The head must then declare a `format`
to be decoded with (Rule 8). A one-segment source reads the whole of another
variable (`data/valid/structured_sources.toml`). A variable with a `source` is
**computed**: it claims no input slot of its own.

```toml
[variables]
frame = { type = "text", format = "json", description = "telemetry frame" }
speed = { type = "number", source = "frame.vehicle.speed" }
```

A dotted source path cannot address an array element, nor a key in the decoded
document that itself contains a dot.

## Monitors

A monitor is a runtime interface: what it reads, what it publishes and how it
is configured. One document may offer several over the same properties
(`data/valid/two_monitors.toml`), each a table keyed by its name:
`[monitors.compact]` is the monitor `compact`, which a tool outside the
document asks for by that name. The name is an identifier, and it is the
monitor's alone: nothing in a rule or another monitor refers to a monitor, so
monitor names share no space with variables, rules or properties.

Monitors are independent of namespaces. Like `variables`, `monitors` sits at
the root alone, and no namespace holds one
(`data/invalid/monitor_in_namespace.toml`): a monitor over a namespace's
properties names them by their paths. A monitor's keys sit under its name, so
the anonymous array the format used to take, `[[monitors]]`, is a type error
(`data/invalid/monitors_as_array.toml`), and so are keys written straight into
`[monitors]` (`data/invalid/monitor_without_name.toml`).

| key | holds |
| --- | --- |
| `inputs` | variables fed in at every step, in slot order |
| `parameters` | variables fixed before the run, in slot order |
| `outputs` | **required.** Values published at every step, in order: properties and computed variables |
| `runtime` | `allocation_size`, `buffer_size`, `base_period`, `time_unit` |

Each list's order *is* that partition's order, so an `inputs` list matching a
trace's column order lets the trace be ingested as it stands. Every key but
`outputs` is optional (`data/valid/partial_monitor.toml`).

- An **input** or **parameter** entry names a variable, bare.
- An **output** entry names a property by its path -- bare in the anonymous
  namespace, `<namespace>.<name>` otherwise -- or a computed variable, bare. A
  rule is never an output: publishing what a rule computes means giving it a
  property of its own.
- `outputs` names at least one value, and a monitor publishes nothing it does
  not name. There is no default set: TOML does not order a table's keys, so
  "every property, in declaration order" would give two loaders two
  interfaces (`data/invalid/monitor_without_outputs.toml`).
- A **parameter** carries its value as `initial_value`; `min` and `max`
  optionally bound it for an embedding that exposes it.

**Deduction.** A document owes neither `[monitors]` nor `[variables]`. A bare
name in a rule that resolves to no rule, property or declared variable can only
be an input, and becomes one, ordered by first use unless an `inputs` list says
otherwise (`data/valid/deduction.toml`). A parameter cannot be deduced: a
threshold has no sensible default, so a parameter needs a declaration with an
`initial_value` (Rule 6). A number written inline in a rule is an anonymous
parameter, allocated a slot after the named ones.

**`runtime`.** `allocation_size` and `buffer_size` size the build. `time_unit`
is the unit of the monitor's timestamps, and the last default of a metric
bound's unit ([Time](#time)). `base_period` is the time between two consecutive
samples, in `time_unit`, so a monitor that sets one sets a `time_unit` too
(`data/invalid/monitor_base_period_without_time_unit.toml`).

**Numbers.** TOML writes `inf`, `-inf` and `nan`. The infinities are numbers
of the format, and `nan` is not (Rule 23).

## Properties

A property is a named requirement, keyed by name under a namespace's
`properties`:

| key | holds |
| --- | --- |
| `check` | **required.** The rule whose satisfaction is the requirement |
| `given` | a rule; `check` is required only where `given` holds |
| `where` | rules private to this property |
| `time_unit` | default unit of the metric bounds in this property |
| `title`, `description`, `message`, `tags` | descriptive only; `message` is reported on violation |
| `extras` | free-form data about the property |

Metadata does not change a property's meaning. Its verdict is defined under
[Verdicts](#verdicts).

## Rules in prefix form

A rule is written either in **prefix form** or in **expression form**. Prefix
form is authoritative: every rule expression form can write has a prefix
spelling, and not the other way round.

A prefix rule is a **reference** -- a string naming a variable, rule or
property ([Names](#names)) -- or an **array**: an operator, its operands, and,
for some operators, a trailing table. The trailing table is what `AGENTS.md`
calls `kwargs`: a metric bound on a temporal operator, a binding on a
quantifier. Nothing else takes one.

The operator alone decides which spelling an array must have, so the schema
reports a malformed rule at the operand at fault rather than at the rule. `eq`
and `ne` belong to both comparison families, and which one a rule is in is a
matter of its operands' types (Rule 20).

| family | spelling | operators |
| --- | --- | --- |
| negation | `[op, rule]` | `not` |
| n-ary | `[op, rule, rule, ...]`, two or more | `and`, `or`, `xor`, `equiv`, `implies` |
| step | `[op, rule]` | `prev`, `next` |
| unary temporal | `[op, rule]` or `[op, rule, bound]` | `once`, `historically`, `eventually`, `always` |
| binary temporal | `[op, rule, rule]` or `[op, rule, rule, bound]` | `since`, `until` |
| number comparison | `[op, variable, variable or number literal]` | `lt`, `le`, `gt`, `ge`, `eq`, `ne` |
| string comparison | `[op, variable, variable or string literal]` | `eq`, `ne`, `contains`, `startswith`, `endswith` |
| assignment | `[assign, variable, quantified variable]` | `assign` |
| quantifier | `[op, rule, binding]` | `forall`, `exists` |

- A **literal** is a table, `{ value = 30 }` or `{ value = "sensor_7" }`, so an
  unmarked operand is always a name.
- A **metric bound** is `{ min = ..., max = ..., time_unit = ... }`. Each key is
  optional, but at least one end is given, and a `time_unit` comes with an
  end. An end is a non-negative number or the bare name of a parameter.
- A **binding** is `{ qvars = ["a", "b"] }`: one or more distinct names.
- A **quantified variable** is `{ qvar = "a" }`, and it stands only as the
  third operand of `assign`.

```toml
[rules]
recent_ack = ["once", "ack", { max = 5, time_unit = "s" }]
held = ["since", "ack", "trigger", { max = "deadline" }]
all_low = ["forall", ["implies", ["assign", "sensor_id", { qvar = "s" }], ["lt", "reading", { value = 120 }]], { qvars = ["s"] }]
```

## Rules in expression form

Expression form is the infix spelling of a rule, written as a TOML string that
begins with `(`. The schema checks only that the string is parenthesised. Its
syntax is the grammar below, and conforming tools reject what it does not
derive.

```text
expression     = "(" quantified ")" ;
quantified     = quantifier binder quantified | implication ;
binder         = "[" name { "," name } "]" ;
implication    = disjunction [ ( "->" | "implies" ) implication ] ;
disjunction    = conjunction { ( "or" | "xor" ) conjunction } ;
conjunction    = temporal { "and" temporal } ;
temporal       = unary { ( "since" | "until" ) [ bound ] unary } ;
unary          = ( "not" | "next" ) unary
               | unary-temporal [ bound ] unary
               | primary ;
primary        = atom | "(" quantified ")" ;
atom           = "{" name "}"
               | "{" name comparison ( name | signed-number ) "}"
               | "{" name ":=" name "}" ;
bound          = "[" end ":" [ end ] [ ":" unit ] "]"
               | "[" ":" end [ ":" unit ] "]" ;
end            = number | name ;

quantifier     = "forall" | "exists" ;
unary-temporal = "once" | "historically" | "eventually" | "always" ;
comparison     = "<" | "<=" | ">" | ">=" | "==" | "!=" ;
unit           = "ns" | "us" | "ms" | "s" | "min" | "h" | "d" ;
name           = ( letter | "_" ) { letter | digit | "_" } ;
number         = digit { digit } [ "." digit { digit } ] [ ( "e" | "E" ) [ "+" | "-" ] digit { digit } ] ;
signed-number  = [ "+" | "-" ] number ;
```

Whitespace may separate any two tokens. Operators bind from tightest to
loosest in the order the grammar nests them: `not` and `next`, the unary
temporal operators, `since` and `until` (left-associative), `and`, `or` and
`xor` (left-associative), `->` and `implies` (right-associative), and the
quantifiers, which extend as far right as they can.

Each construct means its prefix twin:

| expression | prefix |
| --- | --- |
| `{p}` | `"p"` |
| `{speed < 30}`, `{a == b}` | `["lt", "speed", { value = 30 }]`, `["eq", "a", "b"]` |
| `{s := sensor_id}` | `["assign", "sensor_id", { qvar = "s" }]` |
| `not e`, `next e` | `["not", e]`, `["next", e]` |
| `once[2:5:s] e` | `["once", e, { min = 2, max = 5, time_unit = "s" }]` |
| `e since[:5] f` | `["since", e, f, { max = 5 }]` |
| `e and f and g` | `["and", e, f, g]` |
| `e -> f`, `e implies f` | `["implies", e, f]` |
| `forall[a, b] e` | `["forall", e, { qvars = ["a", "b"] }]` |

A chain of `and`, `or`, `xor` or `implies` means the n-ary rule over its
operands: the n-ary definitions below agree with the chained binary ones under
the associativity the grammar gives them.

Expression form is a strict subset. `prev` and `equiv` have no infix spelling
(`data/malformed/prev_has_no_infix_spelling.toml`,
`equiv_has_no_infix_spelling.toml`), a comparison has no string literal, and
a name in braces is never a dotted path. A rule needing any of these is
written in prefix form.

Inside a bound, position alone says which field a token fills, so `[:s]` bounds
by a parameter named `s` while `[1::s]` is at least one second. A bound gives at
least one end: `[3]`, `[:]` and `[::s]` derive nothing. Nothing in a bound is
braced, since a bound holds no atom (`data/malformed/bound_parameter_braced.toml`).

## Semantics

### Traces

A trace is a finite sequence of samples `σ₀ σ₁ … σₙ`. Sample `σᵢ` gives every
input its value at step `i`, and carries a timestamp `τᵢ`, non-decreasing in
`i`. When the monitor sets `base_period`, samples are `base_period` apart:
`τᵢ − τⱼ = (i − j) · base_period`. A parameter holds its `initial_value` at
every step, and a computed variable takes, at every step, the value its
`source` addresses.

A rule `φ` holds or fails at each step `i`, written `i ⊨ φ`.

### Time

A metric bound measures the distance `d(j, i)` between two steps `j ≤ i`. Its
unit is, in this order:

1. the bound's own `time_unit`;
2. the enclosing property's `time_unit`;
3. the monitor's `runtime.time_unit`;
4. otherwise, **steps**.

In a time unit, `d(j, i) = τᵢ − τⱼ` converted to that unit. In steps,
`d(j, i) = i − j`. Every bound is the **closed** interval `[min, max]`: an
absent `min` is `0`, an absent `max` is unbounded, and an end naming a
parameter takes that parameter's value.

### Operators

For a bound `I` (the whole of `[0, ∞)` when no bound is written):

| rule | `i ⊨ rule` iff |
| --- | --- |
| `p` (a `bool` variable) | `p` is true at step `i` |
| a rule or property name | its definition, or its verdict, holds at `i` |
| `not φ` | not `i ⊨ φ` |
| `and φ₁ … φₖ` | every `i ⊨ φₘ` |
| `or φ₁ … φₖ` | some `i ⊨ φₘ` |
| `xor φ₁ … φₖ` | an odd number of the `φₘ` hold at `i` |
| `equiv φ₁ … φₖ` | all the `φₘ` hold at `i`, or none does |
| `implies φ₁ … φₖ` | `φ₁ → (φ₂ → (… → φₖ))` at `i` |
| `prev φ` | `i > 0` and `i−1 ⊨ φ` |
| `next φ` | `i < n` and `i+1 ⊨ φ` |
| `once_I φ` | some `j ≤ i` with `d(j, i) ∈ I` has `j ⊨ φ` |
| `historically_I φ` | every `j ≤ i` with `d(j, i) ∈ I` has `j ⊨ φ` |
| `φ since_I ψ` | some `j ≤ i` with `d(j, i) ∈ I` has `j ⊨ ψ`, and `k ⊨ φ` for every `j < k ≤ i` |
| `eventually_I φ` | some `j ≥ i` with `d(i, j) ∈ I` has `j ⊨ φ` |
| `always_I φ` | every `j ≥ i` with `d(i, j) ∈ I` has `j ⊨ φ` |
| `φ until_I ψ` | some `j ≥ i` with `d(i, j) ∈ I` has `j ⊨ ψ`, and `k ⊨ φ` for every `i ≤ k < j` |

`equiv` over more than two operands is "all equal", which is **not** the chained
biconditional `φ₁ ↔ (φ₂ ↔ φ₃)`. That is also why `equiv` has no infix spelling.
A bound on `since` or `until` constrains when the second operand holds.

**Comparisons.** A number comparison holds at `i` when its relation holds
between the values of its operands at `i`. Its operands are `number` variables
or a number literal. A string comparison does the same over text. `contains`,
`startswith` and `endswith` ask whether the second operand is a substring,
prefix or suffix of the first. Its operands are `text` variables or a string
literal.

### Quantifiers

`forall` and `exists` quantify over values, not over time, and they range over
the **active domain**: the values seen so far. Write `Dᵢ(a)` for the set of
values that the variables assigned to `a` within the quantified rule have
carried at steps `0 … i`. Then:

- `i ⊨ forall[a] φ` iff `i ⊨ φ[a ↦ v]` for every `v ∈ Dᵢ(a)`;
- `i ⊨ exists[a] φ` iff `i ⊨ φ[a ↦ v]` for some `v ∈ Dᵢ(a)`;
- `j ⊨ assign(x, a)` under `a ↦ v` iff `x` has the value `v` at step `j`.

Several names in one binding quantify in turn, left to right. A quantified name
is local to its quantifier: it takes no slot and shadows nothing. Quantification
does not reach through a name: a rule named inside the quantified rule is
resolved where it was declared, where the quantified name is not bound.

### Verdicts

A property yields a **verdict at every step**. At step `i` its verdict is:

- `i ⊨ given → check` when the property has a `given`;
- `i ⊨ check` when it has none.

A monitor publishes that stream for each property among its outputs, and the
value at every step of each computed variable among them. A property named
inside a rule stands for this verdict.

## Time cones

Every operator that looks along the trace looks one way:

| cone | operators |
| --- | --- |
| past | `prev`, `once`, `historically`, `since` |
| future | `next`, `eventually`, `always`, `until` |

Names, comparisons, assignments, quantifiers and the boolean operators have no
direction of their own. A rule built from them takes the cone its operands
share, and a rule built from them alone is neutral -- which is what lets one
shared rule serve a past-time property and a future-time one. A rule whose
operands sit in both cones mixes them and does not conform (Rule 12).

A cone is resolved through names, as far as they go: through `rules`, through a
property's `where` table, and through a property named for its verdict, which
carries the cone of its `check`.

## What the schema cannot check

A document the schema accepts must also obey every rule below. They
are numbered to match the `Rule N` comments in
[`data/semantic/`](data/semantic/), each of which
is schema-valid by construction. Numbers 2, 4 and 10 are **retired**: their
fixtures went with the format changes that made them unwritable. Each rule names
its fixtures: a bare file name is in `data/semantic/`, and any
other gives its path.

1. A name appears in at most one of a monitor's `inputs`, `outputs` and
   `parameters`. -- `monitor_list_overlap.toml`
2. *Retired.*
3. Every monitor entry resolves:
   - An `inputs` entry names a declared variable, or a name a rule uses that
     deduction makes an input. -- `undeclared_monitor_entry.toml`
   - A `parameters` entry names a declared variable.
   - An `outputs` entry names a property or a computed variable, and never a
     rule, private or otherwise. -- `unresolved_output.toml`,
     `output_names_a_rule.toml`, `output_names_a_private_rule.toml`,
     `output_variable_without_source.toml`
4. *Retired.*
5. An `initial_value` sits only on a variable a monitor lists. --
   `initial_value_without_partition.toml`
6. A listed parameter carries an `initial_value`. --
   `parameter_missing_initial_value.toml`
7. `min` is not greater than `max`, on a number variable or on a metric bound.
   -- `min_greater_than_max.toml`, `data/invalid/rule_bound_min_greater_than_max.toml`,
   `data/invalid/operators/{since,until}_bound_min_greater_than_max.toml`
8. A multi-segment `source` path has a declared head that carries a `format`.
   -- `source_undeclared_head.toml`, `data/invalid/source_head_without_format.toml`
9. A `text` or `binary` variable is never read as a truth value or a number;
   a `text` variable may be an operand of a string comparison. --
   `rule_names_text_variable.toml`
10. *Retired.*
11. A name has one source of value where it is visible. Inputs, parameters,
    computed variables, rules, private rules and property verdicts are all
    sources, so no two of them share a bare name within one namespace, and
    none shares a name with a variable. At the root, the name of a top-level
    namespace counts too.
    -- `duplicate_source_of_value.toml`, `rule_and_input_share_name.toml`,
    `rule_and_parameter_share_name.toml`, `property_and_input_share_name.toml`,
    `property_and_private_rule_share_name.toml`,
    `file_rule_and_private_rule_share_name.toml`,
    `data/invalid/rule_shares_name_with_input.toml`
12. No rule mixes the two time cones, whether written out or behind a name. --
    `cone_mixed_through_rule_name.toml`, `cone_mixed_in_a_conjunction.toml`,
    `cone_mixed_through_private_rule.toml`,
    `cone_mixed_through_property_verdict.toml`,
    `data/invalid/mixed_time_cones.toml`, `data/invalid/operators/*_mixed_cone.toml`
13. No rule is defined in terms of itself, directly or around a ring of names.
    -- `rule_defined_in_terms_of_itself.toml`
14. A quantified variable is bound by an enclosing quantifier. --
    `data/invalid/assign_without_quantifier.toml`
15. A nested quantifier does not rebind a name an enclosing quantifier binds. --
    `data/invalid/quantifier_rebinds_enclosing_variable.toml`
16. A quantified name does not reuse the name of a variable, rule or property. --
    `data/invalid/quantifier_shadows_declared_name.toml`
17. Every name a quantifier binds is used by the rule it quantifies. --
    `data/invalid/quantifier_unused_variable.toml`
18. A parameter carries no `source`, being set rather than read. --
    `data/invalid/parameter_with_source.toml`
19. A bound end that is a name resolves to a `number` parameter. --
    `data/invalid/bound_names_text_variable.toml`
20. A comparison's operands have its family's type: `number` for a number
    comparison, `text` for a string comparison
    ([Comparisons](#operators)).
21. A quantified name in an expression-form rule stands only on the left of
    `:=`, as its prefix twin stands only in `assign`.
22. A name -- a bare name, a dotted path or a monitor's output entry --
    resolves to a variable, a rule or a property, and never to a namespace. --
    `reference_to_namespace.toml`
23. No number is `nan`. JSON has no `nan`, and `nan` passes every bound a
    schema can set. --
    `number_is_nan.toml`

The grammar layer has its own fixtures: every file in
[`data/malformed/`](data/malformed/) holds an expression the
[grammar](#rules-in-expression-form) does not derive.

## Open questions

These are undecided, and this text deliberately takes no side on them.

- **Past `given`, future `check`.** May a property pair a past-time `given` with
  a future-time `check` -- the "trigger, then response" pattern? Rule 12 is
  stated over rules, and a property is not one. The deleted `DESIGN.md` said
  `given` "lends the verdict no cone"; nothing since has confirmed it.
- **Units that depend on the monitor.** A bound with no unit of its own, in a
  property with none, takes the monitor's `time_unit`. Two monitors with
  different units therefore give one property two meanings, although monitors
  are meant to be independent of what they run. Unsettled, too: a bound whose
  unit is a time unit, under a monitor that declares no `time_unit` for its
  timestamps.
- **The ends of a finite trace.** [Operators](#operators) reads the boundaries
  strongly: `prev` fails at step `0`, `next` fails at step `n`, and
  `eventually` and `until` fail when the trace ends first. Whether a runtime
  should report that verdict, or withhold one until enough of the future has
  been seen, is undecided.
- **Future-time quantification.** `Dᵢ(a)` collects the values seen up to the
  step evaluated. Whether a quantifier over a future-time rule should range
  over values that appear later is undecided.
- **TOML version.** `AGENTS.md` requires TOML 1.1, but many TOML parsers read
  only 1.0 (Python's `tomllib` on 3.12 among them) and reject 1.1's multi-line
  inline tables, trailing commas and seconds-free times.
- **`source` on any type.** The schema allows `source` on a `bool`, and on a
  variable of any type with any head. Its own description says the source is a
  path "inside a text or binary variable". `flatbuffers` has nowhere to name the
  schema a path into it would need.
- **Formatted text in comparisons.** Whether a `text` variable with a `format`
  may be a string comparison operand, compared as its raw text, is undecided.
- **Dotted names in expressions.** A braced name is an identifier, so expression
  form cannot reach another namespace's rule. Prefix form can.
- **Selecting by tag.** `tags` are described as for selecting properties, but
  no monitor selects by them.
