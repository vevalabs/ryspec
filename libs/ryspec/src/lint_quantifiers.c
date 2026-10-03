/* The checks of quantifiers: Rules 14, 15, 16 and 17.
 *
 * One walk serves the four, carrying the names each enclosing quantifier
 * binds, and reports the violations of the one rule it is asked for. A use
 * marks every binding of its name, so a name rebound (Rule 15) leaves no
 * binding unused (Rule 17) the rebinding has not. */
#include <string.h>

#include "lint.h"

/* A name a quantifier binds, the innermost first. */
typedef struct scope {
  const toml_datum_t *name;
  bool used;
  struct scope *up;
} scope;

typedef struct quantifiers {
  const lint_index *idx;
  const lint_rule *at;
  ryspec_lint_rule rule;
} quantifiers;

static bool same_name(const toml_datum_t *a, const char *b, size_t len) {
  return (size_t)a->u.str.len == len && memcmp(a->u.str.ptr, b, len) == 0;
}

static const toml_datum_t *placed(const toml_datum_t *d,
                                  const toml_datum_t *fallback) {
  return d->lineno ? d : fallback;
}

static ryspec_status walk(const quantifiers *q, const toml_datum_t *v,
                          scope *bound, ryspec_diagnostic *diag);

/* The quantifier v, binding the names of qvars from the i-th on, each
 * pushed onto bound as a frame, then walking its rule operands. */
static ryspec_status bind(const quantifiers *q, const toml_datum_t *v,
                          const toml_datum_t *qvars, int i, scope *bound,
                          ryspec_diagnostic *diag) {
  if (i == qvars->u.arr.size) {
    for (int k = 1; k < v->u.arr.size; k++) {
      ryspec_status s = walk(q, &v->u.arr.elem[k], bound, diag);
      if (s != RYSPEC_OK) {
        return s;
      }
    }
    return RYSPEC_OK;
  }
  const toml_datum_t *name = &qvars->u.arr.elem[i];
  if (name->type != TOML_STRING) {
    return bind(q, v, qvars, i + 1, bound, diag);
  }
  const char *s = name->u.str.ptr;
  size_t len = (size_t)name->u.str.len;
  if (q->rule == RYSPEC_LINT_QVAR_NO_REBIND) {
    for (const scope *b = bound; b; b = b->up) {
      if (same_name(b->name, s, len)) {
        return lint_fail(diag, placed(name, v),
                         "`%s` is already bound by an enclosing quantifier",
                         s);
      }
    }
  }
  if (q->rule == RYSPEC_LINT_QVAR_NO_SHADOW &&
      lint_resolve(q->idx, q->at, s, len).kind != LINT_TO_DEDUCED) {
    return lint_fail(diag, placed(name, v),
                     "quantified name `%s` shadows a declared name", s);
  }
  scope frame = {name, false, bound};
  ryspec_status st = bind(q, v, qvars, i + 1, &frame, diag);
  if (st == RYSPEC_OK && q->rule == RYSPEC_LINT_QVAR_USED && !frame.used) {
    return lint_fail(diag, placed(name, v),
                     "`%s` is never used in the rule it quantifies", s);
  }
  return st;
}

/* assign's quantified variable, { qvar = "a" }: a use of a. */
static ryspec_status use(const quantifiers *q, const toml_datum_t *t,
                         scope *bound, const toml_datum_t *v,
                         ryspec_diagnostic *diag) {
  const toml_datum_t *name = ryspec_toml_value_lookup(t, "qvar", 4);
  if (!name || name->type != TOML_STRING) {
    return RYSPEC_OK;
  }
  bool found = false;
  for (scope *b = bound; b; b = b->up) {
    if (same_name(b->name, name->u.str.ptr, (size_t)name->u.str.len)) {
      b->used = true;
      found = true;
    }
  }
  if (!found && q->rule == RYSPEC_LINT_QVAR_BOUND) {
    return lint_fail(diag, placed(t, v),
                     "`%s` is a quantified variable, and an enclosing "
                     "quantifier binds no such name",
                     name->u.s);
  }
  return RYSPEC_OK;
}

static ryspec_status walk(const quantifiers *q, const toml_datum_t *v,
                          scope *bound, ryspec_diagnostic *diag) {
  if (v->type != TOML_ARRAY || v->u.arr.size == 0) {
    return RYSPEC_OK;
  }
  const toml_datum_t *head = &v->u.arr.elem[0];
  ryspec_rule_op op = RYSPEC_RULE_OP_NOT;
  bool known = head->type == TOML_STRING &&
               ryspec_rule_op_from_name(head->u.s, &op);
  if (known && (op == RYSPEC_RULE_OP_FORALL || op == RYSPEC_RULE_OP_EXISTS)) {
    const toml_datum_t *last = &v->u.arr.elem[v->u.arr.size - 1];
    const toml_datum_t *qvars = ryspec_toml_value_lookup(last, "qvars", 5);
    if (qvars && qvars->type == TOML_ARRAY) {
      return bind(q, v, qvars, 0, bound, diag);
    }
  }
  for (int i = 1; i < v->u.arr.size; i++) {
    const toml_datum_t *e = &v->u.arr.elem[i];
    ryspec_status s = e->type == TOML_TABLE
                          ? (known && op == RYSPEC_RULE_OP_ASSIGN
                                 ? use(q, e, bound, v, diag)
                                 : RYSPEC_OK)
                          : walk(q, e, bound, diag);
    if (s != RYSPEC_OK) {
      return s;
    }
  }
  return RYSPEC_OK;
}

static ryspec_status check(const lint_index *idx, ryspec_lint_rule rule,
                           ryspec_diagnostic *diag) {
  for (size_t i = 0; i < idx->n_rules; i++) {
    quantifiers q = {idx, &idx->rules[i], rule};
    ryspec_status s = walk(&q, idx->rules[i].value, NULL, diag);
    if (s != RYSPEC_OK) {
      return s;
    }
  }
  return RYSPEC_OK;
}

/* Rule 14: a quantified variable is bound by an enclosing quantifier. */
ryspec_status lint_check_qvar_bound(const lint_index *idx,
                                    ryspec_diagnostic *diag) {
  return check(idx, RYSPEC_LINT_QVAR_BOUND, diag);
}

/* Rule 15: a nested quantifier does not rebind a name an enclosing one
 * binds. */
ryspec_status lint_check_qvar_no_rebind(const lint_index *idx,
                                        ryspec_diagnostic *diag) {
  return check(idx, RYSPEC_LINT_QVAR_NO_REBIND, diag);
}

/* Rule 16: a quantified name does not reuse the name of a variable, a rule
 * or a property, as seen from where it is bound. */
ryspec_status lint_check_qvar_no_shadow(const lint_index *idx,
                                        ryspec_diagnostic *diag) {
  return check(idx, RYSPEC_LINT_QVAR_NO_SHADOW, diag);
}

/* Rule 17: every name a quantifier binds is used by the rule it
 * quantifies. */
ryspec_status lint_check_qvar_used(const lint_index *idx,
                                   ryspec_diagnostic *diag) {
  return check(idx, RYSPEC_LINT_QVAR_USED, diag);
}
