/* The linter: the walker, and the checks dispatched by rule. */
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "lint.h"

static const toml_datum_t *get(const toml_datum_t *t, const char *key) {
  return ryspec_toml_value_lookup(t, key, strlen(key));
}

/* ---------------------------------------------------------------------------
 * The walker. */

static const toml_datum_t *placed_at(const toml_datum_t *d,
                                     const toml_datum_t *fallback) {
  return d->lineno ? d : fallback;
}

/* What a table operand is, by the key it holds. */
static lint_node_kind table_kind(const toml_datum_t *t) {
  if (get(t, "value")) {
    return LINT_LITERAL;
  }
  if (get(t, "qvar")) {
    return LINT_QVAR;
  }
  if (get(t, "qvars")) {
    return LINT_BINDING;
  }
  return LINT_BOUND;
}

static ryspec_status walk(const toml_datum_t *v, const lint_node *parent,
                          ryspec_entity position, lint_visit fn, void *ctx,
                          ryspec_diagnostic *diag) {
  lint_node n = {.datum = v, .parent = parent, .position = position};
  const toml_datum_t *fallback = parent ? parent->at : v;
  n.at = placed_at(v, fallback);
  if (v->type == TOML_STRING) {
    n.kind = LINT_REFERENCE;
    return fn(&n, ctx, diag);
  }
  if (v->type == TOML_TABLE) {
    n.kind = table_kind(v);
    if (parent) {
      n.op = parent->op;
      n.known = parent->known;
    }
    return fn(&n, ctx, diag);
  }
  if (v->type != TOML_ARRAY || v->u.arr.size == 0) {
    return RYSPEC_OK;
  }
  const toml_datum_t *head = &v->u.arr.elem[0];
  n.kind = LINT_OPERATOR;
  n.known =
      head->type == TOML_STRING && ryspec_rule_op_from_name(head->u.s, &n.op);
  ryspec_status s = fn(&n, ctx, diag);
  for (int i = 1; s == RYSPEC_OK && i < v->u.arr.size; i++) {
    s = walk(&v->u.arr.elem[i], &n, position, fn, ctx, diag);
  }
  return s;
}

ryspec_status lint_walk(const toml_datum_t *value, lint_visit fn, void *ctx,
                        ryspec_diagnostic *diag) {
  return walk(value, NULL, RYSPEC_NO_ENTITY, fn, ctx, diag);
}

ryspec_status lint_walk_rule(const ryspec_toml_doc *doc, ryspec_entity position,
                             lint_visit fn, void *ctx,
                             ryspec_diagnostic *diag) {
  return walk(index_node(doc, position), NULL, position, fn, ctx, diag);
}

ryspec_status lint_walk_all(const ryspec_toml_doc *doc, lint_visit fn,
                            void *ctx, ryspec_diagnostic *diag) {
  ryspec_status s = RYSPEC_OK;
  for (ryspec_entity e = 0; s == RYSPEC_OK && e < doc->n_entities; e++) {
    if (index_is_position(doc->entities[e].kind)) {
      s = lint_walk_rule(doc, e, fn, ctx, diag);
    }
  }
  return s;
}

bool lint_op_is_comparison(ryspec_rule_op op) {
  return op >= RYSPEC_RULE_OP_LT && op <= RYSPEC_RULE_OP_ENDSWITH;
}

bool lint_node_is_truth(const lint_node *n) {
  const lint_node *p = n->parent;
  return !p || !p->known ||
         !(lint_op_is_comparison(p->op) || p->op == RYSPEC_RULE_OP_ASSIGN);
}

/* ---------------------------------------------------------------------------
 * Diagnostics. */

ryspec_status lint_vfail(ryspec_diagnostic *diag, ryspec_status status,
                         const toml_datum_t *at, const char *fmt, va_list ap) {
  if (diag) {
    diag->status = status;
    diag->line = at ? at->lineno : 0;
    diag->column = at ? at->colno : 0;
    vsnprintf(diag->message, sizeof diag->message, fmt, ap);
  }
  return status;
}

ryspec_status lint_fail_status(ryspec_diagnostic *diag, ryspec_status status,
                               const toml_datum_t *at, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  ryspec_status s = lint_vfail(diag, status, at, fmt, ap);
  va_end(ap);
  return s;
}

ryspec_status lint_fail(ryspec_diagnostic *diag, const toml_datum_t *at,
                        const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  ryspec_status s = lint_vfail(diag, RYSPEC_ERROR_SEMANTIC, at, fmt, ap);
  va_end(ap);
  return s;
}

void lint_number_text(const toml_datum_t *d, char *buf, size_t size) {
  if (d->type == TOML_INT64) {
    snprintf(buf, size, "%" PRId64, d->u.int64);
  } else {
    snprintf(buf, size, "%g", d->u.fp64);
  }
}

/* ---------------------------------------------------------------------------
 * The checks, by rule, in the order ryspec_toml_lint() runs them. */

static const struct {
  ryspec_lint_rule rule;
  lint_check fn;
} checks[] = {
    {RYSPEC_LINT_SCHEMA, lint_check_schema},
    {RYSPEC_LINT_MONITOR_LIST_OVERLAP, lint_check_monitor_list_overlap},
    {RYSPEC_LINT_MONITOR_ENTRY_RESOLVES, lint_check_monitor_entry_resolves},
    {RYSPEC_LINT_INITIAL_VALUE_LISTED, lint_check_initial_value_listed},
    {RYSPEC_LINT_PARAMETER_INITIAL, lint_check_parameter_initial},
    {RYSPEC_LINT_MIN_LE_MAX, lint_check_min_le_max},
    {RYSPEC_LINT_SOURCE_HEAD, lint_check_source_head},
    {RYSPEC_LINT_NO_TEXT_AS_TRUTH, lint_check_no_text_as_truth},
    {RYSPEC_LINT_ONE_SOURCE_OF_VALUE, lint_check_one_source_of_value},
    {RYSPEC_LINT_ONE_CONE, lint_check_one_cone},
    {RYSPEC_LINT_NO_CYCLE, lint_check_no_cycle},
    {RYSPEC_LINT_QVAR_BOUND, lint_check_qvar_bound},
    {RYSPEC_LINT_QVAR_NO_REBIND, lint_check_qvar_no_rebind},
    {RYSPEC_LINT_QVAR_NO_SHADOW, lint_check_qvar_no_shadow},
    {RYSPEC_LINT_QVAR_USED, lint_check_qvar_used},
    {RYSPEC_LINT_PARAMETER_NO_SOURCE, lint_check_parameter_no_source},
    {RYSPEC_LINT_BOUND_NAME_NUMBER, lint_check_bound_name_number},
    {RYSPEC_LINT_COMPARISON_TYPES, lint_check_comparison_types},
    {RYSPEC_LINT_NO_NAMESPACE_REFERENCE, lint_check_no_namespace_reference},
    {RYSPEC_LINT_NO_NAN, lint_check_no_nan},
};

#define N_CHECKS (sizeof checks / sizeof checks[0])

/* The rules of checks[], in its order; the test holds the two together. */
static const ryspec_lint_rule rules[N_CHECKS] = {
    RYSPEC_LINT_SCHEMA,
    RYSPEC_LINT_MONITOR_LIST_OVERLAP,
    RYSPEC_LINT_MONITOR_ENTRY_RESOLVES,
    RYSPEC_LINT_INITIAL_VALUE_LISTED,
    RYSPEC_LINT_PARAMETER_INITIAL,
    RYSPEC_LINT_MIN_LE_MAX,
    RYSPEC_LINT_SOURCE_HEAD,
    RYSPEC_LINT_NO_TEXT_AS_TRUTH,
    RYSPEC_LINT_ONE_SOURCE_OF_VALUE,
    RYSPEC_LINT_ONE_CONE,
    RYSPEC_LINT_NO_CYCLE,
    RYSPEC_LINT_QVAR_BOUND,
    RYSPEC_LINT_QVAR_NO_REBIND,
    RYSPEC_LINT_QVAR_NO_SHADOW,
    RYSPEC_LINT_QVAR_USED,
    RYSPEC_LINT_PARAMETER_NO_SOURCE,
    RYSPEC_LINT_BOUND_NAME_NUMBER,
    RYSPEC_LINT_COMPARISON_TYPES,
    RYSPEC_LINT_NO_NAMESPACE_REFERENCE,
    RYSPEC_LINT_NO_NAN,
};

const ryspec_lint_rule *ryspec_lint_rules(size_t *count) {
  *count = N_CHECKS;
  return rules;
}

/* Run the checks [first, last) over doc, stopping at the first violation. */
static ryspec_status run(const ryspec_toml_doc *doc, size_t first, size_t last,
                         ryspec_diagnostic *diag) {
  ryspec_status s = RYSPEC_OK;
  for (size_t i = first; s == RYSPEC_OK && i < last; i++) {
    s = checks[i].fn(doc, diag);
  }
  if (s == RYSPEC_OK) {
    ryspec_diagnose(diag, RYSPEC_OK, 0, 0, "");
  }
  return s;
}

ryspec_status ryspec_toml_lint(const ryspec_toml_doc *doc,
                               ryspec_diagnostic *diag) {
  return run(doc, 0, N_CHECKS, diag);
}

ryspec_status ryspec_toml_lint_rule(const ryspec_toml_doc *doc,
                                    ryspec_lint_rule rule,
                                    ryspec_diagnostic *diag) {
  for (size_t i = 0; i < N_CHECKS; i++) {
    if (checks[i].rule == rule) {
      return run(doc, i, i + 1, diag);
    }
  }
  ryspec_diagnose(diag, RYSPEC_ERROR_SEMANTIC, 0, 0, "no lint rule %d",
                  (int)rule);
  return RYSPEC_ERROR_SEMANTIC;
}
