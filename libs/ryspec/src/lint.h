/*
 * The linter, private to libryspec: the rules of SPEC.md's "What the schema
 * cannot check", one check per rule, each of which runs alone.
 *
 * A lint indexes the document once: its namespaces, rules, properties,
 * variables and monitors, each pointing into tomlc17's tree, copying
 * nothing. A check reads the index and reports its rule's first violation,
 * placed in the document. No check assumes another passed, nor that the
 * document is schema-valid: a datum of the wrong type is passed over.
 * Nothing under an `extras` table is indexed, walked or reported.
 */
#ifndef RYSPEC_SRC_LINT_H
#define RYSPEC_SRC_LINT_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#include "expr_parser.h"
#include "toml_doc.h"

/* ---------------------------------------------------------------------------
 * The index. */

/* A namespace: the root, the anonymous one, at index 0, its parent -1 and
 * its name NULL; a named one by its last segment under its parent. */
typedef struct lint_namespace {
  int parent;
  const char *name;
  int len;
  const toml_datum_t *table;
} lint_namespace;

typedef struct lint_property {
  int ns;
  const char *name;
  int len;
  const toml_datum_t *table;
} lint_property;

/* What a rule position is: a rule of a namespace, a rule private to a
 * property, or a property's `given` or `check`. */
typedef enum lint_rule_kind {
  LINT_RULE,
  LINT_PRIVATE_RULE,
  LINT_GIVEN,
  LINT_CHECK,
} lint_rule_kind;

/* A rule position. name is NULL for a `given` or a `check`, and property
 * is -1 for a rule of a namespace. */
typedef struct lint_rule {
  lint_rule_kind kind;
  int ns;
  int property;
  const char *name;
  int len;
  const toml_datum_t *value;
} lint_rule;

typedef enum lint_type {
  LINT_BOOL,
  LINT_NUMBER,
  LINT_TEXT,
  LINT_BINARY,
} lint_type;

typedef struct lint_variable {
  const char *name;
  int len;
  lint_type type;
  const toml_datum_t *decl;
} lint_variable;

/* A monitor; a list it lacks, or one that is no array, is NULL. */
typedef struct lint_monitor {
  const char *name;
  int len;
  const toml_datum_t *table;
  const toml_datum_t *inputs, *parameters, *outputs;
} lint_monitor;

typedef struct lint_index {
  lint_namespace *namespaces;
  lint_property *properties;
  lint_rule *rules;
  lint_variable *variables;
  lint_monitor *monitors;
  size_t n_namespaces, n_properties, n_rules, n_variables, n_monitors;
} lint_index;

/* Index doc into *idx. Returns RYSPEC_OK, or RYSPEC_ERROR_MEMORY with diag,
 * which may be NULL, filled and *idx only fit to be freed. */
ryspec_status lint_index_build(const ryspec_toml_doc *doc, lint_index *idx,
                               ryspec_diagnostic *diag);

void lint_index_free(lint_index *idx);

/* The variable declared as the len bytes at name, or NULL. */
const lint_variable *lint_variable_find(const lint_index *idx,
                                        const char *name, size_t len);

/* Whether the array a, which may be NULL, holds the string of len bytes at
 * name. */
bool lint_array_holds(const toml_datum_t *a, const char *name, size_t len);

/* Whether monitor m lists the len bytes at name in any of its lists. */
bool lint_monitor_lists(const lint_monitor *m, const char *name, size_t len);

/* ---------------------------------------------------------------------------
 * The walker. */

typedef enum lint_node_kind {
  LINT_REFERENCE, /* a name: a string */
  LINT_OPERATOR,  /* an array [op, operands..., keywords] */
  LINT_LITERAL,   /* { value = ... } */
  LINT_QVAR,      /* { qvar = "a" } */
  LINT_BOUND,     /* { min = ..., max = ..., time_unit = ... } */
  LINT_BINDING,   /* { qvars = [...] } */
} lint_node_kind;

/* A node of a rule. op is the operator's, for an operator, and for a bound
 * or binding the operator it belongs to; known says whether its name is
 * one. at is a place for a diagnostic: the node's own, or where the node
 * has none, its operator's. parent is the operator the node is an operand
 * of, or NULL at the top of the rule; rule is the rule position walked, or
 * NULL when the walk is of a value alone. */
typedef struct lint_node lint_node;
struct lint_node {
  lint_node_kind kind;
  const toml_datum_t *datum;
  ryspec_rule_op op;
  bool known;
  const toml_datum_t *at;
  const lint_node *parent;
  const lint_rule *rule;
};

/* Whether the node is read as a truth value: a rule's top, or an operand
 * of an operator other than a comparison or assign. */
bool lint_node_is_truth(const lint_node *n);

/* Whether op is a comparison, of either family. */
bool lint_op_is_comparison(ryspec_rule_op op);

/* Receives each node, in prefix order; anything but RYSPEC_OK stops the
 * walk and is returned. */
typedef ryspec_status (*lint_visit)(const lint_node *n, void *ctx,
                                    ryspec_diagnostic *diag);

/* Walk the rule at value. */
ryspec_status lint_walk(const toml_datum_t *value, lint_visit fn, void *ctx,
                        ryspec_diagnostic *diag);

/* Walk the rule at position r. */
ryspec_status lint_walk_rule(const lint_rule *r, lint_visit fn, void *ctx,
                             ryspec_diagnostic *diag);

/* Walk every rule position of the document, in index order. */
ryspec_status lint_walk_all(const lint_index *idx, lint_visit fn, void *ctx,
                            ryspec_diagnostic *diag);

/* ---------------------------------------------------------------------------
 * Names (SPEC.md's "Names"). */

typedef enum lint_target_kind {
  LINT_TO_NOTHING,   /* a dotted path nothing answers to */
  LINT_TO_DEDUCED,   /* a bare name nothing declares: an input */
  LINT_TO_VARIABLE,  /* index into variables */
  LINT_TO_RULE,      /* index into rules, of a namespace or private */
  LINT_TO_PROPERTY,  /* index into properties */
  LINT_TO_NAMESPACE, /* index into namespaces */
  LINT_TO_AMBIGUOUS, /* more than one of the above: Rule 11's */
} lint_target_kind;

typedef struct lint_target {
  lint_target_kind kind;
  size_t index;
} lint_target;

/* What the len bytes at name, written in the rule position at, resolve to.
 * A bare name is looked up in the `where` table of at's property, at's
 * namespace's rules and properties, the variables, and at the root the
 * top-level namespaces; a dotted path from the root. at NULL is the root,
 * outside any property. */
lint_target lint_resolve(const lint_index *idx, const lint_rule *at,
                         const char *name, size_t len);

/* The type a reference has, when it resolves to a variable, a rule or a
 * property, a rule's or property's being LINT_BOOL, in *out, returning
 * whether it has one. */
bool lint_target_type(const lint_index *idx, lint_target t, lint_type *out);

/* The name of the type, as "text". */
const char *lint_type_name(lint_type t);

/* ---------------------------------------------------------------------------
 * Diagnostics. */

/* Fill diag, which may be NULL, with status at datum, and return status. */
ryspec_status lint_vfail(ryspec_diagnostic *diag, ryspec_status status,
                         const toml_datum_t *at, const char *fmt, va_list ap);

ryspec_status lint_fail_status(ryspec_diagnostic *diag, ryspec_status status,
                               const toml_datum_t *at, const char *fmt, ...);

/* Fill diag, which may be NULL, with RYSPEC_ERROR_SEMANTIC at datum, and
 * return that status. */
ryspec_status lint_fail(ryspec_diagnostic *diag, const toml_datum_t *at,
                        const char *fmt, ...);

/* Write the number datum d, an integer or a float, into buf. */
void lint_number_text(const toml_datum_t *d, char *buf, size_t size);

/* ---------------------------------------------------------------------------
 * The checks, one per rule (lint_monitors.c, lint_variables.c,
 * lint_names.c, lint_cones.c, lint_quantifiers.c), the schema's first
 * (lint_schema.c). */

typedef ryspec_status (*lint_check)(const lint_index *idx,
                                    ryspec_diagnostic *diag);

ryspec_status lint_check_schema(const lint_index *idx,
                                ryspec_diagnostic *diag);
ryspec_status lint_check_monitor_list_overlap(const lint_index *idx,
                                              ryspec_diagnostic *diag);
ryspec_status lint_check_monitor_entry_resolves(const lint_index *idx,
                                               ryspec_diagnostic *diag);
ryspec_status lint_check_initial_value_listed(const lint_index *idx,
                                              ryspec_diagnostic *diag);
ryspec_status lint_check_parameter_initial(const lint_index *idx,
                                           ryspec_diagnostic *diag);
ryspec_status lint_check_min_le_max(const lint_index *idx,
                                    ryspec_diagnostic *diag);
ryspec_status lint_check_source_head(const lint_index *idx,
                                    ryspec_diagnostic *diag);
ryspec_status lint_check_no_text_as_truth(const lint_index *idx,
                                          ryspec_diagnostic *diag);
ryspec_status lint_check_one_source_of_value(const lint_index *idx,
                                             ryspec_diagnostic *diag);
ryspec_status lint_check_one_cone(const lint_index *idx,
                                  ryspec_diagnostic *diag);
ryspec_status lint_check_no_cycle(const lint_index *idx,
                                  ryspec_diagnostic *diag);
ryspec_status lint_check_qvar_bound(const lint_index *idx,
                                    ryspec_diagnostic *diag);
ryspec_status lint_check_qvar_no_rebind(const lint_index *idx,
                                        ryspec_diagnostic *diag);
ryspec_status lint_check_qvar_no_shadow(const lint_index *idx,
                                        ryspec_diagnostic *diag);
ryspec_status lint_check_qvar_used(const lint_index *idx,
                                   ryspec_diagnostic *diag);
ryspec_status lint_check_parameter_no_source(const lint_index *idx,
                                             ryspec_diagnostic *diag);
ryspec_status lint_check_bound_name_number(const lint_index *idx,
                                          ryspec_diagnostic *diag);
ryspec_status lint_check_comparison_types(const lint_index *idx,
                                          ryspec_diagnostic *diag);
ryspec_status lint_check_no_namespace_reference(const lint_index *idx,
                                                ryspec_diagnostic *diag);
ryspec_status lint_check_no_nan(const lint_index *idx,
                                ryspec_diagnostic *diag);

#endif /* RYSPEC_SRC_LINT_H */
