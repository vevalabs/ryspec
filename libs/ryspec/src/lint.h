/*
 * The linter, private to libryspec: the rules of SPEC.md's "What the schema
 * cannot check", one check per rule, each of which runs alone.
 *
 * A check reads the document's tree, its entities by toml_doc.h's visitor
 * and its references by its resolver, walking the rules of its positions,
 * and reports its rule's first violation, placed in the document. No check
 * assumes another passed, nor that the document is schema-valid: a datum of
 * the wrong type is passed over. Nothing under an `extras` table is visited,
 * walked or reported.
 */
#ifndef RYSPEC_SRC_LINT_H
#define RYSPEC_SRC_LINT_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#include "expr_parser.h"
#include "toml_doc.h"

/* ---------------------------------------------------------------------------
 * The rules. */

/* What the linter checks, by number: the schema, as 0, then the rules of
 * SPEC.md's "What the schema cannot check", each named by its number
 * there. */
enum {
  RYSPEC_LINT_SCHEMA = 0,                  /* schemas/v0/, by hand */
  RYSPEC_LINT_MONITOR_LIST_OVERLAP = 1,    /* a name in one monitor list */
  RYSPEC_LINT_MONITOR_ENTRY_RESOLVES = 3,  /* every monitor entry resolves */
  RYSPEC_LINT_INITIAL_VALUE_LISTED = 5,    /* initial_value on listed names */
  RYSPEC_LINT_PARAMETER_INITIAL = 6,       /* a parameter has initial_value */
  RYSPEC_LINT_MIN_LE_MAX = 7,              /* min no greater than max */
  RYSPEC_LINT_SOURCE_HEAD = 8,             /* a source's head has a format */
  RYSPEC_LINT_NO_TEXT_AS_TRUTH = 9,        /* text, binary never a truth or
                                              a number */
  RYSPEC_LINT_ONE_SOURCE_OF_VALUE = 11,    /* one source per visible name */
  RYSPEC_LINT_ONE_CONE = 12,               /* no rule mixes time cones */
  RYSPEC_LINT_NO_CYCLE = 13,               /* no rule defined by itself */
  RYSPEC_LINT_QVAR_BOUND = 14,             /* a qvar has its quantifier */
  RYSPEC_LINT_QVAR_NO_REBIND = 15,         /* no quantifier rebinds a name */
  RYSPEC_LINT_QVAR_NO_SHADOW = 16,         /* no qvar takes a declared name */
  RYSPEC_LINT_QVAR_USED = 17,              /* every bound name is used */
  RYSPEC_LINT_PARAMETER_NO_SOURCE = 18,    /* a parameter has no source */
  RYSPEC_LINT_BOUND_NAME_NUMBER = 19,      /* a bound name is a number
                                              parameter */
  RYSPEC_LINT_COMPARISON_TYPES = 20,       /* operands fit the comparison */
  RYSPEC_LINT_NO_NAMESPACE_REFERENCE = 22, /* no name is a namespace */
  RYSPEC_LINT_NO_NAN = 23,                 /* no number is nan */
};

/* The rules ryspec_toml_lint() checks, in the order it checks them; *count
 * is set to how many. */
const int* ryspec_lint_rules(size_t* count);

/* Lint doc against the one rule, as ryspec_toml_lint() does. A rule other
 * than the schema presumes a document the schema accepts. A rule the linter
 * does not check fails with RYSPEC_ERROR_SEMANTIC, unplaced. */
int ryspec_lint_rule(const ryspec_toml_doc* doc, int rule, ryspec_diag* diag);

/* ---------------------------------------------------------------------------
 * The walker. */

typedef enum ryspec_lint_node_kind {
  RYSPEC_LINT_REFERENCE, /* a name: a string */
  RYSPEC_LINT_OPERATOR,  /* an array [op, operands..., keywords] */
  RYSPEC_LINT_LITERAL,   /* { value = ... } */
  RYSPEC_LINT_QVAR,      /* { qvar = "a" } */
  RYSPEC_LINT_BOUND,     /* { min = ..., max = ..., time_unit = ... } */
  RYSPEC_LINT_BINDING,   /* { qvars = [...] } */
} ryspec_lint_node_kind;

/* A node of a rule. op is the operator's, for an operator, and for a bound
 * or binding the operator it belongs to; known says whether its name is
 * one. at is a place for a diagnostic: the node's own, or where the node
 * has none, its operator's. parent is the operator the node is an operand
 * of, or NULL at the top of the rule; position is the rule position walked,
 * or NULL when the walk is of a value alone. */
typedef struct ryspec_lint_node ryspec_lint_node;
struct ryspec_lint_node {
  ryspec_lint_node_kind kind;
  const toml_datum_t* datum;
  ryspec_rule_op op;
  bool known;
  const toml_datum_t* at;
  const ryspec_lint_node* parent;
  const ryspec_toml_doc_entity* position;
};

/* Whether the node is read as a truth value: a rule's top, or an operand
 * of an operator other than a comparison or assign. */
bool ryspec_lint_node_is_truth(const ryspec_lint_node* n);

/* Whether op is a comparison, of either family. */
bool ryspec_lint_op_is_comparison(ryspec_rule_op op);

/* Receives each node, in prefix order; anything but RYSPEC_OK stops the
 * walk and is returned. */
typedef int (*ryspec_lint_visit)(
  const ryspec_lint_node* n, void* ctx, ryspec_diag* diag);

/* Walk the rule at value. */
int ryspec_lint_walk(
  const toml_datum_t* value,
  ryspec_lint_visit fn,
  void* ctx,
  ryspec_diag* diag);

/* Walk the rule at the rule position. */
int ryspec_lint_walk_rule(
  const ryspec_toml_doc_entity* position,
  ryspec_lint_visit fn,
  void* ctx,
  ryspec_diag* diag);

/* Walk every rule position of the document, in its order. */
int ryspec_lint_walk_all(
  const ryspec_toml_doc* doc,
  ryspec_lint_visit fn,
  void* ctx,
  ryspec_diag* diag);

/* ---------------------------------------------------------------------------
 * Diagnostics. */

/* Fill diag, which may be NULL, with status at datum, and return status. */
int ryspec_lint_vfail(
  ryspec_diag* diag,
  int status,
  const toml_datum_t* at,
  const char* fmt,
  va_list ap);

int ryspec_lint_fail_status(
  ryspec_diag* diag, int status, const toml_datum_t* at, const char* fmt, ...);

/* Fill diag, which may be NULL, with RYSPEC_ERROR_SEMANTIC at datum, and
 * return that status. */
int ryspec_lint_fail(
  ryspec_diag* diag, const toml_datum_t* at, const char* fmt, ...);

/* Write the number datum d, an integer or a float, into buf. */
void ryspec_lint_number_text(const toml_datum_t* d, char* buf, size_t size);

/* ---------------------------------------------------------------------------
 * The checks (lint_check.c): the schema's, then one per rule. */

typedef int (*ryspec_lint_check)(const ryspec_toml_doc* doc, ryspec_diag* diag);

int ryspec_lint_check_schema(const ryspec_toml_doc* doc, ryspec_diag* diag);
int ryspec_lint_check_monitor_list_overlap(
  const ryspec_toml_doc* doc, ryspec_diag* diag);
int ryspec_lint_check_monitor_entry_resolves(
  const ryspec_toml_doc* doc, ryspec_diag* diag);
int ryspec_lint_check_initial_value_listed(
  const ryspec_toml_doc* doc, ryspec_diag* diag);
int ryspec_lint_check_parameter_initial(
  const ryspec_toml_doc* doc, ryspec_diag* diag);
int ryspec_lint_check_min_le_max(const ryspec_toml_doc* doc, ryspec_diag* diag);
int ryspec_lint_check_source_head(
  const ryspec_toml_doc* doc, ryspec_diag* diag);
int ryspec_lint_check_no_text_as_truth(
  const ryspec_toml_doc* doc, ryspec_diag* diag);
int ryspec_lint_check_one_source_of_value(
  const ryspec_toml_doc* doc, ryspec_diag* diag);
int ryspec_lint_check_one_cone(const ryspec_toml_doc* doc, ryspec_diag* diag);
int ryspec_lint_check_no_cycle(const ryspec_toml_doc* doc, ryspec_diag* diag);
int ryspec_lint_check_qvar_bound(const ryspec_toml_doc* doc, ryspec_diag* diag);
int ryspec_lint_check_qvar_no_rebind(
  const ryspec_toml_doc* doc, ryspec_diag* diag);
int ryspec_lint_check_qvar_no_shadow(
  const ryspec_toml_doc* doc, ryspec_diag* diag);
int ryspec_lint_check_qvar_used(const ryspec_toml_doc* doc, ryspec_diag* diag);
int ryspec_lint_check_parameter_no_source(
  const ryspec_toml_doc* doc, ryspec_diag* diag);
int ryspec_lint_check_bound_name_number(
  const ryspec_toml_doc* doc, ryspec_diag* diag);
int ryspec_lint_check_comparison_types(
  const ryspec_toml_doc* doc, ryspec_diag* diag);
int ryspec_lint_check_no_namespace_reference(
  const ryspec_toml_doc* doc, ryspec_diag* diag);
int ryspec_lint_check_no_nan(const ryspec_toml_doc* doc, ryspec_diag* diag);

#endif /* RYSPEC_SRC_LINT_H */
