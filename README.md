# Reelay Specification Format

The Reelay Specification (`ryspec`) Format is a declarative specification format focused on expressing multiple temporal logic properties. It is based on the TOML specification format and is designed to provide a simple, human-readable representation of temporal logic specifications consumed by runtime verification tools and reasoning agents.

A `ryspec` document can contain any number of temporal logic properties. Each property is declared as a TOML table under `properties`, keyed by its own name -- `[properties.my_property]` below -- and specifies a temporal logic formula through the `check` field. A document may divide its properties into named namespaces, written `[<namespace>.properties.<name>]`; the examples here declare none, which is the anonymous namespace and is what [Document Structure](SPEC.md#document-structure) covers.

A single property can be expressed as follows:

```toml
[properties.my_property]
check = "({s} -> once[3:10] {p})"
```

Multiple properties can be specified in the same document:

```toml
[properties.my_property]
check = "({s} -> once[3:10] {p})"

[properties.other_property]
check = "((once[:10] {q}) -> ((not {p}) since {q}))"
```

Each property is evaluated independently while sharing the signals referenced by its temporal logic formula.

The language itself -- document structure, both rule syntaxes, what a rule means, and the rules the schema cannot check -- is defined in [SPEC.md](SPEC.md).
