# Reelay Specification Format

The Reelay Specification (`ryspec`) Format is a declarative specification format focused on expressing multiple temporal logic properties. It is based on the TOML specification format and is designed to provide a simple, human-readable representation of temporal logic specifications consumed by runtime verification tools and reasoning agents.

A `ryspec` document can contain one or more temporal logic properties. Each property is declared using a TOML `[[properties]]` array-of-tables entry and specifies a temporal logic formula through the `check` field.

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

Expression form is also a subset of prefix form. Comparisons such as `["gt", "speed", "speed_max"]`, the `prev` operator, and `equiv` have no infix spelling, so a property needing any of them writes that rule in prefix form.

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

## Document Structure

Nine top-level keys, of which `version` and `properties` are required:

| key | holds |
| --- | --- |
| `version` | **required.** The ryspec schema version this document targets -- `0`, today |
| `properties` | **required.** The properties, each yielding one verdict |
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

## Check with schema

Install the validator with `pip install git+<repo-url>`, which provides a
`ryspec` command. `ryspec validate` checks every `*.toml` under a directory
against the bundled schema:

```sh
ryspec validate data
```

It exits non-zero on the first file that fails, reporting the JSON Pointer to the
offending value. Beyond schema validation, `ryspec validate` also performs the
checks listed under "What the schema cannot check" above, reporting whichever
of the two fails first. `--expect-invalid` inverts the assertion, for a
directory of negative fixtures. Editors read the `#:schema` header at the top
of each file for the same checks inline.

See [`examples/`](examples/) for a working `*.toml` file per topic covered above --
`ryspec validate examples` checks them all against the schema.