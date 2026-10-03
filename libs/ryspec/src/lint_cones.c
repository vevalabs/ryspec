/* The checks that follow names into their definitions: Rules 12 and 13. */
#include <stdio.h>
#include <string.h>

#include "lint.h"

/* The rule positions a reference written at at leads to, each passed to fn
 * until it returns anything but RYSPEC_OK: a rule's, or a property's
 * `given` and `check`, or with check_only its `check` alone. */
typedef ryspec_status (*position_fn)(size_t position, void *ctx,
                                     ryspec_diagnostic *diag);

static ryspec_status each_definition(const lint_index *idx,
                                     const lint_rule *at,
                                     const toml_datum_t *name, bool check_only,
                                     position_fn fn, void *ctx,
                                     ryspec_diagnostic *diag) {
  lint_target t =
      lint_resolve(idx, at, name->u.str.ptr, (size_t)name->u.str.len);
  if (t.kind == LINT_TO_RULE) {
    return fn(t.index, ctx, diag);
  }
  if (t.kind != LINT_TO_PROPERTY) {
    return RYSPEC_OK;
  }
  for (size_t i = 0; i < idx->n_rules; i++) {
    const lint_rule *r = &idx->rules[i];
    bool wanted = r->kind == LINT_CHECK || (!check_only && r->kind == LINT_GIVEN);
    if (wanted && r->property == (int)t.index) {
      ryspec_status s = fn(i, ctx, diag);
      if (s != RYSPEC_OK) {
        return s;
      }
    }
  }
  return RYSPEC_OK;
}

/* A rule's name, or for a `given` or `check` its property's. */
static void position_name(const lint_index *idx, size_t i, const char **name,
                          int *len) {
  const lint_rule *r = &idx->rules[i];
  if (r->name) {
    *name = r->name;
    *len = r->len;
  } else {
    *name = idx->properties[r->property].name;
    *len = idx->properties[r->property].len;
  }
}

/* Per rule position: not yet visited, being visited, visited. */
enum { UNSEEN, OPEN, DONE };

static unsigned char *new_states(const lint_index *idx) {
  size_t n = idx->n_rules ? idx->n_rules : 1;
  unsigned char *states = ryspec_toml_options().mem_realloc(NULL, n);
  if (states) {
    memset(states, UNSEEN, n);
  }
  return states;
}

static ryspec_status out_of_memory(ryspec_diagnostic *diag) {
  ryspec_diagnose(diag, RYSPEC_ERROR_MEMORY, 0, 0, "out of memory");
  return RYSPEC_ERROR_MEMORY;
}

/* ---------------------------------------------------------------------------
 * Rule 13: no rule is defined in terms of itself, directly or around a ring
 * of names. A property named for its verdict is defined by its `given` and
 * its `check`. Reported at the reference that closes the ring. */

typedef struct ring {
  const lint_index *idx;
  unsigned char *states;
  const toml_datum_t *at; /* the reference followed */
} ring;

static ryspec_status visit(size_t position, ring *r, ryspec_diagnostic *diag);

static ryspec_status step(size_t position, void *ctx,
                          ryspec_diagnostic *diag) {
  ring *r = ctx;
  if (r->states[position] == OPEN) {
    const char *name;
    int len;
    position_name(r->idx, position, &name, &len);
    return lint_fail(diag, r->at, "`%.*s` is defined in terms of itself", len,
                     name);
  }
  return r->states[position] == UNSEEN ? visit(position, r, diag) : RYSPEC_OK;
}

static ryspec_status follow(const lint_node *n, void *ctx,
                            ryspec_diagnostic *diag) {
  ring *r = ctx;
  if (n->kind != LINT_REFERENCE) {
    return RYSPEC_OK;
  }
  r->at = n->at;
  return each_definition(r->idx, n->rule, n->datum, false, step, r, diag);
}

static ryspec_status visit(size_t position, ring *r, ryspec_diagnostic *diag) {
  r->states[position] = OPEN;
  ryspec_status s = lint_walk_rule(&r->idx->rules[position], follow, r, diag);
  r->states[position] = DONE;
  return s;
}

ryspec_status lint_check_no_cycle(const lint_index *idx,
                                  ryspec_diagnostic *diag) {
  ring r = {idx, new_states(idx), NULL};
  if (!r.states) {
    return out_of_memory(diag);
  }
  ryspec_status s = RYSPEC_OK;
  for (size_t i = 0; s == RYSPEC_OK && i < idx->n_rules; i++) {
    if (r.states[i] == UNSEEN) {
      s = visit(i, &r, diag);
    }
  }
  ryspec_toml_options().mem_free(r.states);
  return s;
}

/* ---------------------------------------------------------------------------
 * Rule 12: no rule mixes the two time cones, written out or behind a name.
 * A name takes the cone of what it resolves to: a rule's definition, or a
 * property's `check`. A name already being resolved, around a ring Rule 13
 * refuses, is taken as neutral. */

typedef enum cone {
  CONE_NEUTRAL,
  CONE_PAST,
  CONE_FUTURE,
} cone;

static const char *cone_name(cone c) {
  return c == CONE_PAST ? "past" : "future";
}

static cone cone_of_op(ryspec_rule_op op) {
  switch (op) {
  case RYSPEC_RULE_OP_PREV:
  case RYSPEC_RULE_OP_ONCE:
  case RYSPEC_RULE_OP_HISTORICALLY:
  case RYSPEC_RULE_OP_SINCE:
    return CONE_PAST;
  case RYSPEC_RULE_OP_NEXT:
  case RYSPEC_RULE_OP_EVENTUALLY:
  case RYSPEC_RULE_OP_ALWAYS:
  case RYSPEC_RULE_OP_UNTIL:
    return CONE_FUTURE;
  default:
    return CONE_NEUTRAL;
  }
}

typedef struct cones {
  const lint_index *idx;
  unsigned char *states;
  cone *of; /* per rule position, once DONE */
  cone found; /* what each_definition() last led to */
} cones;

static ryspec_status cone_of(cones *c, const toml_datum_t *v,
                             const lint_rule *at, cone *out,
                             ryspec_diagnostic *diag);

static ryspec_status position_cone(size_t position, void *ctx,
                                   ryspec_diagnostic *diag) {
  cones *c = ctx;
  if (c->states[position] == UNSEEN) {
    c->states[position] = OPEN;
    const lint_rule *r = &c->idx->rules[position];
    ryspec_status s = cone_of(c, r->value, r, &c->of[position], diag);
    c->states[position] = DONE;
    if (s != RYSPEC_OK) {
      return s;
    }
  }
  c->found = c->states[position] == DONE ? c->of[position] : CONE_NEUTRAL;
  return RYSPEC_OK;
}

/* How a diagnostic names the i-th operand of the n rule operands of v. */
static void describe(const toml_datum_t *e, int i, int n, char *buf,
                     size_t size) {
  if (e->type == TOML_STRING) {
    snprintf(buf, size, "'%s'", e->u.s);
  } else if (n == 1) {
    snprintf(buf, size, "its operand");
  } else {
    snprintf(buf, size, "operand %d", i);
  }
}

static ryspec_status cone_of(cones *c, const toml_datum_t *v,
                             const lint_rule *at, cone *out,
                             ryspec_diagnostic *diag) {
  *out = CONE_NEUTRAL;
  if (v->type == TOML_STRING) {
    c->found = CONE_NEUTRAL;
    ryspec_status s =
        each_definition(c->idx, at, v, true, position_cone, c, diag);
    *out = c->found;
    return s;
  }
  if (v->type != TOML_ARRAY || v->u.arr.size == 0) {
    return RYSPEC_OK;
  }
  const toml_datum_t *head = &v->u.arr.elem[0];
  ryspec_rule_op op = RYSPEC_RULE_OP_NOT;
  bool known = head->type == TOML_STRING &&
               ryspec_rule_op_from_name(head->u.s, &op);
  if (known && (lint_op_is_comparison(op) || op == RYSPEC_RULE_OP_ASSIGN)) {
    return RYSPEC_OK; /* names of values: no direction */
  }
  cone own = known ? cone_of_op(op) : CONE_NEUTRAL;
  const char *op_name = known ? ryspec_rule_op_name(op) : head->u.s;
  int operands = 0;
  for (int i = 1; i < v->u.arr.size; i++) {
    operands += v->u.arr.elem[i].type != TOML_TABLE;
  }
  cone shared = CONE_NEUTRAL;
  const toml_datum_t *first = NULL;
  int first_i = 0;
  for (int i = 1, k = 0; i < v->u.arr.size; i++) {
    const toml_datum_t *e = &v->u.arr.elem[i];
    if (e->type == TOML_TABLE) {
      continue; /* a bound or a binding */
    }
    k++;
    cone oc;
    ryspec_status s = cone_of(c, e, at, &oc, diag);
    if (s != RYSPEC_OK) {
      return s;
    }
    if (oc == CONE_NEUTRAL) {
      continue;
    }
    char what[160];
    describe(e, k, operands, what, sizeof what);
    const toml_datum_t *place = e->lineno ? e : v;
    if (own != CONE_NEUTRAL) {
      if (oc != own) {
        return lint_fail(diag, place,
                         "'%s' looks to the %s and %s is a %s-time rule",
                         op_name, cone_name(own), what, cone_name(oc));
      }
      continue;
    }
    if (shared == CONE_NEUTRAL) {
      shared = oc;
      first = e;
      first_i = k;
    } else if (oc != shared) {
      char first_what[160];
      describe(first, first_i, operands, first_what, sizeof first_what);
      return lint_fail(diag, place,
                       "in '%s', %s is a %s-time rule and the operand at "
                       "position %d is a %s-time rule",
                       op_name, first_what, cone_name(shared), k,
                       cone_name(oc));
    }
  }
  *out = own != CONE_NEUTRAL ? own : shared;
  return RYSPEC_OK;
}

ryspec_status lint_check_one_cone(const lint_index *idx,
                                  ryspec_diagnostic *diag) {
  size_t n = idx->n_rules ? idx->n_rules : 1;
  cones c = {idx, new_states(idx),
             ryspec_toml_options().mem_realloc(NULL, n * sizeof(cone)),
             CONE_NEUTRAL};
  ryspec_status s = RYSPEC_OK;
  if (!c.states || !c.of) {
    s = out_of_memory(diag);
  }
  for (size_t i = 0; s == RYSPEC_OK && i < idx->n_rules; i++) {
    s = position_cone(i, &c, diag);
  }
  ryspec_toml_options().mem_free(c.states);
  ryspec_toml_options().mem_free(c.of);
  return s;
}
