/*
 * The linter, private to libryspec: the rules of SPEC.md's "What the schema
 * cannot check", one check per rule, each of which runs alone.
 *
 * A check reads the document's index (toml_doc.h), built as it was parsed,
 * walking the rules of its positions, and reports its rule's first violation,
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
 * of, or NULL at the top of the rule; position is the rule position walked,
 * or RYSPEC_NO_ENTITY when the walk is of a value alone. */
typedef struct lint_node lint_node;
struct lint_node {
  lint_node_kind kind;
  const toml_datum_t *datum;
  ryspec_rule_op op;
  bool known;
  const toml_datum_t *at;
  const lint_node *parent;
  ryspec_entity position;
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

/* Walk the rule at the rule position of doc. */
ryspec_status lint_walk_rule(const ryspec_toml_doc *doc, ryspec_entity position,
                             lint_visit fn, void *ctx, ryspec_diagnostic *diag);

/* Walk every rule position of the document, in id order. */
ryspec_status lint_walk_all(const ryspec_toml_doc *doc, lint_visit fn,
                            void *ctx, ryspec_diagnostic *diag);

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
 * The checks (lint_check.c): the schema's, then one per rule. */

typedef ryspec_status (*lint_check)(const ryspec_toml_doc *doc,
                                    ryspec_diagnostic *diag);

ryspec_status lint_check_schema(const ryspec_toml_doc *doc,
                                ryspec_diagnostic *diag);
ryspec_status lint_check_monitor_list_overlap(const ryspec_toml_doc *doc,
                                              ryspec_diagnostic *diag);
ryspec_status lint_check_monitor_entry_resolves(const ryspec_toml_doc *doc,
                                                ryspec_diagnostic *diag);
ryspec_status lint_check_initial_value_listed(const ryspec_toml_doc *doc,
                                              ryspec_diagnostic *diag);
ryspec_status lint_check_parameter_initial(const ryspec_toml_doc *doc,
                                           ryspec_diagnostic *diag);
ryspec_status lint_check_min_le_max(const ryspec_toml_doc *doc,
                                    ryspec_diagnostic *diag);
ryspec_status lint_check_source_head(const ryspec_toml_doc *doc,
                                     ryspec_diagnostic *diag);
ryspec_status lint_check_no_text_as_truth(const ryspec_toml_doc *doc,
                                          ryspec_diagnostic *diag);
ryspec_status lint_check_one_source_of_value(const ryspec_toml_doc *doc,
                                             ryspec_diagnostic *diag);
ryspec_status lint_check_one_cone(const ryspec_toml_doc *doc,
                                  ryspec_diagnostic *diag);
ryspec_status lint_check_no_cycle(const ryspec_toml_doc *doc,
                                  ryspec_diagnostic *diag);
ryspec_status lint_check_qvar_bound(const ryspec_toml_doc *doc,
                                    ryspec_diagnostic *diag);
ryspec_status lint_check_qvar_no_rebind(const ryspec_toml_doc *doc,
                                        ryspec_diagnostic *diag);
ryspec_status lint_check_qvar_no_shadow(const ryspec_toml_doc *doc,
                                        ryspec_diagnostic *diag);
ryspec_status lint_check_qvar_used(const ryspec_toml_doc *doc,
                                   ryspec_diagnostic *diag);
ryspec_status lint_check_parameter_no_source(const ryspec_toml_doc *doc,
                                             ryspec_diagnostic *diag);
ryspec_status lint_check_bound_name_number(const ryspec_toml_doc *doc,
                                           ryspec_diagnostic *diag);
ryspec_status lint_check_comparison_types(const ryspec_toml_doc *doc,
                                          ryspec_diagnostic *diag);
ryspec_status lint_check_no_namespace_reference(const ryspec_toml_doc *doc,
                                                ryspec_diagnostic *diag);
ryspec_status lint_check_no_nan(const ryspec_toml_doc *doc,
                                ryspec_diagnostic *diag);

#endif /* RYSPEC_SRC_LINT_H */
