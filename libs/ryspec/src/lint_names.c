/* The checks of names and their types: Rules 9, 11, 19, 20 and 22. */
#include <string.h>

#include "lint.h"

static const toml_datum_t *get(const toml_datum_t *t, const char *key) {
  return ryspec_toml_value_lookup(t, key, strlen(key));
}

static lint_target resolve(const lint_index *idx, const lint_node *n,
                           const toml_datum_t *name) {
  return lint_resolve(idx, n->rule, name->u.str.ptr, (size_t)name->u.str.len);
}

/* ---------------------------------------------------------------------------
 * Comparisons: the family a comparison is in, by its operator, and for `eq`
 * and `ne`, by its operands' types. */

typedef enum family {
  FAMILY_UNKNOWN,
  FAMILY_NUMBER,
  FAMILY_TEXT,
} family;

static family family_of(const lint_index *idx, const lint_node *n) {
  switch (n->op) {
  case RYSPEC_RULE_OP_LT:
  case RYSPEC_RULE_OP_LE:
  case RYSPEC_RULE_OP_GT:
  case RYSPEC_RULE_OP_GE:
    return FAMILY_NUMBER;
  case RYSPEC_RULE_OP_CONTAINS:
  case RYSPEC_RULE_OP_STARTSWITH:
  case RYSPEC_RULE_OP_ENDSWITH:
    return FAMILY_TEXT;
  default:
    break;
  }
  family f = FAMILY_UNKNOWN;
  for (int i = 1; i < n->datum->u.arr.size; i++) {
    const toml_datum_t *e = &n->datum->u.arr.elem[i];
    lint_type type;
    if (e->type == TOML_TABLE) {
      const toml_datum_t *value = get(e, "value");
      if (!value) {
        continue;
      }
      type = value->type == TOML_STRING ? LINT_TEXT : LINT_NUMBER;
    } else if (e->type != TOML_STRING ||
               !lint_target_type(idx, resolve(idx, n, e), &type)) {
      continue;
    }
    if (type == LINT_NUMBER) {
      return FAMILY_NUMBER;
    }
    if (type == LINT_TEXT) {
      f = FAMILY_TEXT;
    }
  }
  return f;
}

/* Each operand of the comparison n naming something of a type, with it:
 * fn is called on each until it returns anything but RYSPEC_OK. */
typedef ryspec_status (*operand_fn)(const lint_node *n, family f,
                                    const toml_datum_t *e, lint_type type,
                                    ryspec_diagnostic *diag);

static ryspec_status each_operand(const lint_index *idx, const lint_node *n,
                                  operand_fn fn, ryspec_diagnostic *diag) {
  family f = family_of(idx, n);
  for (int i = 1; i < n->datum->u.arr.size; i++) {
    const toml_datum_t *e = &n->datum->u.arr.elem[i];
    lint_type type;
    if (e->type != TOML_STRING ||
        !lint_target_type(idx, resolve(idx, n, e), &type)) {
      continue;
    }
    ryspec_status s = fn(n, f, e, type, diag);
    if (s != RYSPEC_OK) {
      return s;
    }
  }
  return RYSPEC_OK;
}

static bool is_comparison(const lint_node *n) {
  return n->kind == LINT_OPERATOR && n->known && lint_op_is_comparison(n->op);
}

static const toml_datum_t *placed(const toml_datum_t *e, const lint_node *n) {
  return e->lineno ? e : n->at;
}

/* ---------------------------------------------------------------------------
 * Rule 9: a text or binary variable is never read as a truth value, nor as a
 * number, which is as an operand of a number comparison. */

static ryspec_status not_read_as_number(const lint_node *n, family f,
                                        const toml_datum_t *e, lint_type type,
                                        ryspec_diagnostic *diag) {
  if (f != FAMILY_NUMBER || (type != LINT_TEXT && type != LINT_BINARY)) {
    return RYSPEC_OK;
  }
  return lint_fail(diag, placed(e, n),
                   "`%s` is a %s variable, and `%s` reads it as a number",
                   e->u.s, lint_type_name(type), ryspec_rule_op_name(n->op));
}

static ryspec_status not_text_as_truth(const lint_node *n, void *ctx,
                                       ryspec_diagnostic *diag) {
  const lint_index *idx = ctx;
  if (is_comparison(n)) {
    return each_operand(idx, n, not_read_as_number, diag);
  }
  if (n->kind != LINT_REFERENCE || !lint_node_is_truth(n)) {
    return RYSPEC_OK;
  }
  lint_target t = resolve(idx, n, n->datum);
  lint_type type;
  if (t.kind != LINT_TO_VARIABLE || !lint_target_type(idx, t, &type) ||
      (type != LINT_TEXT && type != LINT_BINARY)) {
    return RYSPEC_OK;
  }
  return lint_fail(diag, n->at,
                   "`%s` is a %s variable, and is read as a truth value",
                   n->datum->u.s, lint_type_name(type));
}

ryspec_status lint_check_no_text_as_truth(const lint_index *idx,
                                          ryspec_diagnostic *diag) {
  return lint_walk_all(idx, not_text_as_truth, (void *)idx, diag);
}

/* ---------------------------------------------------------------------------
 * Rule 11: no two sources of value share a bare name where both are
 * visible: within one namespace its rules, its properties and the rules
 * private to each property; any of those and a variable; and at the root,
 * a top-level namespace and any of the others. */

typedef struct source {
  const char *what;
  const char *name;
  int len;
  int ns;       /* the namespace it is in, or -1 for a variable */
  int property; /* the property it is private to, or -1 */
  const toml_datum_t *at;
} source;

/* Whether a and b are both visible where either is. */
static bool collide(const source *a, const source *b) {
  if (a->len != b->len || memcmp(a->name, b->name, (size_t)a->len) != 0) {
    return false;
  }
  if (a->ns < 0 || b->ns < 0) {
    return true; /* a variable is visible everywhere */
  }
  if (a->ns != b->ns) {
    return false;
  }
  return a->property < 0 || b->property < 0 || a->property == b->property;
}

/* The later of the two in the document, where a diagnostic belongs. */
static const toml_datum_t *later(const toml_datum_t *a,
                                 const toml_datum_t *b) {
  if (!a->lineno) {
    return b;
  }
  if (!b->lineno) {
    return a;
  }
  if (a->lineno != b->lineno) {
    return a->lineno > b->lineno ? a : b;
  }
  return a->colno >= b->colno ? a : b;
}

/* The i-th source of the document: its variables, rules, properties and,
 * as sources of the root, its top-level namespaces. */
static bool source_at(const lint_index *idx, size_t i, source *out) {
  if (i < idx->n_variables) {
    const lint_variable *v = &idx->variables[i];
    *out = (source){"a variable", v->name, v->len, -1, -1, v->decl};
    return true;
  }
  i -= idx->n_variables;
  if (i < idx->n_rules) {
    const lint_rule *r = &idx->rules[i];
    if (!r->name) {
      out->name = NULL; /* a given or a check, no source */
      return true;
    }
    bool private = r->kind == LINT_PRIVATE_RULE;
    *out = (source){private ? "a private rule" : "a rule",
                    r->name,
                    r->len,
                    r->ns,
                    private ? r->property : -1,
                    r->value};
    return true;
  }
  i -= idx->n_rules;
  if (i < idx->n_properties) {
    const lint_property *p = &idx->properties[i];
    *out = (source){"a property", p->name, p->len, p->ns, -1, p->table};
    return true;
  }
  i -= idx->n_properties;
  if (i + 1 < idx->n_namespaces) {
    const lint_namespace *n = &idx->namespaces[i + 1];
    out->name = NULL;
    if (n->parent == 0) {
      *out = (source){"a namespace", n->name, n->len, 0, -1, n->table};
    }
    return true;
  }
  return false;
}

ryspec_status lint_check_one_source_of_value(const lint_index *idx,
                                             ryspec_diagnostic *diag) {
  source a, b;
  for (size_t i = 0; source_at(idx, i, &a); i++) {
    if (!a.name) {
      continue;
    }
    for (size_t j = 0; j < i && source_at(idx, j, &b); j++) {
      if (!b.name || (a.ns < 0 && b.ns < 0) || !collide(&a, &b)) {
        continue;
      }
      bool a_later = later(a.at, b.at) == a.at;
      return lint_fail(diag, a_later ? a.at : b.at,
                       "`%.*s` has more than one source of value: %s and %s",
                       a.len, a.name, a_later ? b.what : a.what,
                       a_later ? a.what : b.what);
    }
  }
  return RYSPEC_OK;
}

/* ---------------------------------------------------------------------------
 * Rule 19: a bound end that is a name resolves to a number parameter. */

static ryspec_status bound_names_number(const lint_node *n, void *ctx,
                                        ryspec_diagnostic *diag) {
  const lint_index *idx = ctx;
  if (n->kind != LINT_BOUND) {
    return RYSPEC_OK;
  }
  static const char *ends[] = {"min", "max"};
  for (int i = 0; i < 2; i++) {
    const toml_datum_t *end = get(n->datum, ends[i]);
    if (!end || end->type != TOML_STRING) {
      continue;
    }
    lint_target t = resolve(idx, n, end);
    if (t.kind == LINT_TO_AMBIGUOUS) {
      continue; /* Rule 11's */
    }
    lint_type type;
    if (t.kind == LINT_TO_VARIABLE && lint_target_type(idx, t, &type) &&
        type == LINT_NUMBER) {
      continue;
    }
    if (t.kind == LINT_TO_VARIABLE) {
      return lint_fail(diag, placed(end, n),
                       "the bound's %s `%s` is a %s variable, with no "
                       "comparable value: a bound names a number parameter",
                       ends[i], end->u.s, lint_type_name(type));
    }
    return lint_fail(diag, placed(end, n),
                     "the bound's %s `%s` names no number parameter, so has "
                     "no comparable value",
                     ends[i], end->u.s);
  }
  return RYSPEC_OK;
}

ryspec_status lint_check_bound_name_number(const lint_index *idx,
                                           ryspec_diagnostic *diag) {
  return lint_walk_all(idx, bound_names_number, (void *)idx, diag);
}

/* ---------------------------------------------------------------------------
 * Rule 20: a comparison's operands have its family's type. A text or binary
 * operand of a number comparison is Rule 9's. */

static ryspec_status fits_family(const lint_node *n, family f,
                                 const toml_datum_t *e, lint_type type,
                                 ryspec_diagnostic *diag) {
  const char *op = ryspec_rule_op_name(n->op);
  switch (f) {
  case FAMILY_NUMBER:
    if (type == LINT_NUMBER || type == LINT_TEXT || type == LINT_BINARY) {
      return RYSPEC_OK;
    }
    return lint_fail(diag, placed(e, n),
                     "`%s` is a %s, and `%s` here compares numbers", e->u.s,
                     lint_type_name(type), op);
  case FAMILY_TEXT:
    if (type == LINT_TEXT) {
      return RYSPEC_OK;
    }
    return lint_fail(diag, placed(e, n),
                     "`%s` is a %s, and `%s` here compares text", e->u.s,
                     lint_type_name(type), op);
  default:
    return lint_fail(diag, placed(e, n),
                     "`%s` is a %s, and `%s` compares numbers or text",
                     e->u.s, lint_type_name(type), op);
  }
}

static ryspec_status comparison_fits(const lint_node *n, void *ctx,
                                     ryspec_diagnostic *diag) {
  if (!is_comparison(n)) {
    return RYSPEC_OK;
  }
  return each_operand(ctx, n, fits_family, diag);
}

ryspec_status lint_check_comparison_types(const lint_index *idx,
                                          ryspec_diagnostic *diag) {
  return lint_walk_all(idx, comparison_fits, (void *)idx, diag);
}

/* ---------------------------------------------------------------------------
 * Rule 22: a name resolves to a variable, a rule or a property, and never
 * to a namespace; a dotted path, which deduction never makes an input,
 * resolves to one of them too. */

static ryspec_status names_no_namespace(const lint_node *n, void *ctx,
                                        ryspec_diagnostic *diag) {
  const lint_index *idx = ctx;
  if (n->kind != LINT_REFERENCE) {
    return RYSPEC_OK;
  }
  lint_target t = resolve(idx, n, n->datum);
  if (t.kind == LINT_TO_NAMESPACE) {
    return lint_fail(diag, n->at,
                     "`%s` names a namespace, and a name resolves to a "
                     "variable, a rule or a property",
                     n->datum->u.s);
  }
  if (t.kind == LINT_TO_NOTHING) {
    return lint_fail(diag, n->at,
                     "`%s` resolves to no rule and no property",
                     n->datum->u.s);
  }
  return RYSPEC_OK;
}

ryspec_status lint_check_no_namespace_reference(const lint_index *idx,
                                                ryspec_diagnostic *diag) {
  for (size_t i = 0; i < idx->n_monitors; i++) {
    const lint_monitor *m = &idx->monitors[i];
    for (int k = 0; m->outputs && k < m->outputs->u.arr.size; k++) {
      const toml_datum_t *e = &m->outputs->u.arr.elem[k];
      if (e->type == TOML_STRING &&
          lint_resolve(idx, NULL, e->u.s, (size_t)e->u.str.len).kind ==
              LINT_TO_NAMESPACE) {
        return lint_fail(diag, e,
                         "output `%s` of monitor `%.*s` names a namespace",
                         e->u.s, m->len, m->name);
      }
    }
  }
  return lint_walk_all(idx, names_no_namespace, (void *)idx, diag);
}
