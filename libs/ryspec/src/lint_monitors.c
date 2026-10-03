/* The checks of monitors: Rules 1, 5, 6 and 18. */
#include <string.h>

#include "lint.h"

/* Rule 1: a name appears in at most one of a monitor's lists. Reported at
 * its entry in the later list. */
ryspec_status lint_check_monitor_list_overlap(const lint_index *idx,
                                              ryspec_diagnostic *diag) {
  static const char *names[] = {"inputs", "parameters", "outputs"};
  for (size_t i = 0; i < idx->n_monitors; i++) {
    const lint_monitor *m = &idx->monitors[i];
    const toml_datum_t *lists[] = {m->inputs, m->parameters, m->outputs};
    for (int b = 1; b < 3; b++) {
      for (int k = 0; lists[b] && k < lists[b]->u.arr.size; k++) {
        const toml_datum_t *e = &lists[b]->u.arr.elem[k];
        if (e->type != TOML_STRING) {
          continue;
        }
        for (int a = 0; a < b; a++) {
          if (lint_array_holds(lists[a], e->u.str.ptr,
                               (size_t)e->u.str.len)) {
            return lint_fail(diag, e,
                             "`%s` is in both the %s and the %s of monitor "
                             "`%.*s`",
                             e->u.s, names[a], names[b], m->len, m->name);
          }
        }
      }
    }
  }
  return RYSPEC_OK;
}

/* Rule 5: an initial_value sits only on a variable a monitor lists. */
ryspec_status lint_check_initial_value_listed(const lint_index *idx,
                                              ryspec_diagnostic *diag) {
  for (size_t i = 0; i < idx->n_variables; i++) {
    const lint_variable *v = &idx->variables[i];
    const toml_datum_t *initial =
        ryspec_toml_value_lookup(v->decl, "initial_value", 13);
    if (!initial) {
      continue;
    }
    bool listed = false;
    for (size_t j = 0; !listed && j < idx->n_monitors; j++) {
      listed = lint_monitor_lists(&idx->monitors[j], v->name, (size_t)v->len);
    }
    if (!listed) {
      return lint_fail(diag, initial,
                       "`%.*s` has an initial_value, but no monitor lists it",
                       v->len, v->name);
    }
  }
  return RYSPEC_OK;
}

/* Each declared variable a parameters entry names, with the entry: fn is
 * called on each until it returns anything but RYSPEC_OK. */
typedef ryspec_status (*parameter_fn)(const lint_monitor *m,
                                      const toml_datum_t *entry,
                                      const lint_variable *v,
                                      ryspec_diagnostic *diag);

static ryspec_status each_parameter(const lint_index *idx, parameter_fn fn,
                                    ryspec_diagnostic *diag) {
  for (size_t i = 0; i < idx->n_monitors; i++) {
    const lint_monitor *m = &idx->monitors[i];
    for (int k = 0; m->parameters && k < m->parameters->u.arr.size; k++) {
      const toml_datum_t *e = &m->parameters->u.arr.elem[k];
      if (e->type != TOML_STRING) {
        continue;
      }
      const lint_variable *v =
          lint_variable_find(idx, e->u.str.ptr, (size_t)e->u.str.len);
      if (!v) {
        continue; /* Rule 3's */
      }
      ryspec_status s = fn(m, e, v, diag);
      if (s != RYSPEC_OK) {
        return s;
      }
    }
  }
  return RYSPEC_OK;
}

static ryspec_status has_initial(const lint_monitor *m,
                                 const toml_datum_t *entry,
                                 const lint_variable *v,
                                 ryspec_diagnostic *diag) {
  if (ryspec_toml_value_lookup(v->decl, "initial_value", 13)) {
    return RYSPEC_OK;
  }
  return lint_fail(diag, entry,
                   "`%.*s` is a parameter of monitor `%.*s` and has no "
                   "initial_value",
                   v->len, v->name, m->len, m->name);
}

/* Rule 6: a listed parameter carries an initial_value. Reported at its
 * entry. */
ryspec_status lint_check_parameter_initial(const lint_index *idx,
                                           ryspec_diagnostic *diag) {
  return each_parameter(idx, has_initial, diag);
}

static ryspec_status has_no_source(const lint_monitor *m,
                                   const toml_datum_t *entry,
                                   const lint_variable *v,
                                   ryspec_diagnostic *diag) {
  (void)entry;
  const toml_datum_t *source = ryspec_toml_value_lookup(v->decl, "source", 6);
  if (!source) {
    return RYSPEC_OK;
  }
  return lint_fail(diag, source,
                   "`%.*s` is a parameter of monitor `%.*s` and may not "
                   "declare source",
                   v->len, v->name, m->len, m->name);
}

/* Rule 18: a parameter carries no `source`. Reported at the source. */
ryspec_status lint_check_parameter_no_source(const lint_index *idx,
                                             ryspec_diagnostic *diag) {
  return each_parameter(idx, has_no_source, diag);
}

/* Whether a bare reference of some rule, of len bytes at name, resolves to
 * nothing declared, so that deduction makes it an input. */
typedef struct deduced {
  const lint_index *idx;
  const char *name;
  size_t len;
} deduced;

static ryspec_status is_deduced(const lint_node *n, void *ctx,
                                ryspec_diagnostic *diag) {
  (void)diag;
  const deduced *d = ctx;
  if (n->kind != LINT_REFERENCE || (size_t)n->datum->u.str.len != d->len ||
      memcmp(n->datum->u.str.ptr, d->name, d->len) != 0) {
    return RYSPEC_OK;
  }
  lint_target t = lint_resolve(d->idx, n->rule, d->name, d->len);
  /* Any status but RYSPEC_OK stops the walk: found. */
  return t.kind == LINT_TO_DEDUCED ? RYSPEC_ERROR_SEMANTIC : RYSPEC_OK;
}

static bool used_as_input(const lint_index *idx, const char *name,
                          size_t len) {
  deduced d = {idx, name, len};
  return lint_walk_all(idx, is_deduced, &d, NULL) != RYSPEC_OK;
}

static ryspec_status output_resolves(const lint_index *idx,
                                     const lint_monitor *m,
                                     const toml_datum_t *e,
                                     ryspec_diagnostic *diag) {
  const char *name = e->u.str.ptr;
  size_t len = (size_t)e->u.str.len;
  lint_target t = lint_resolve(idx, NULL, name, len);
  switch (t.kind) {
  case LINT_TO_PROPERTY:
  case LINT_TO_NAMESPACE: /* Rule 22's */
  case LINT_TO_AMBIGUOUS: /* Rule 11's */
    return RYSPEC_OK;
  case LINT_TO_RULE:
    return lint_fail(diag, e,
                     "output `%s` of monitor `%.*s` names a rule, and a "
                     "monitor publishes properties and computed variables",
                     name, m->len, m->name);
  case LINT_TO_VARIABLE:
    if (ryspec_toml_value_lookup(idx->variables[t.index].decl, "source", 6)) {
      return RYSPEC_OK;
    }
    return lint_fail(diag, e,
                     "output `%s` of monitor `%.*s` names a variable with no "
                     "source, which nothing computes",
                     name, m->len, m->name);
  default:
    return lint_fail(diag, e,
                     "output `%s` of monitor `%.*s` names no property or "
                     "computed variable",
                     name, m->len, m->name);
  }
}

/* Rule 3: every monitor entry resolves. An input names a declared variable
 * or a name deduction makes an input, a parameter a declared variable, and
 * an output a property or a computed variable. */
ryspec_status lint_check_monitor_entry_resolves(const lint_index *idx,
                                                ryspec_diagnostic *diag) {
  for (size_t i = 0; i < idx->n_monitors; i++) {
    const lint_monitor *m = &idx->monitors[i];
    const toml_datum_t *lists[] = {m->inputs, m->parameters, m->outputs};
    static const char *what[] = {"input", "parameter"};
    for (int l = 0; l < 3; l++) {
      for (int k = 0; lists[l] && k < lists[l]->u.arr.size; k++) {
        const toml_datum_t *e = &lists[l]->u.arr.elem[k];
        if (e->type != TOML_STRING) {
          continue;
        }
        const char *name = e->u.str.ptr;
        size_t len = (size_t)e->u.str.len;
        if (l == 2) {
          ryspec_status s = output_resolves(idx, m, e, diag);
          if (s != RYSPEC_OK) {
            return s;
          }
          continue;
        }
        if (lint_variable_find(idx, name, len) ||
            (l == 0 && used_as_input(idx, name, len))) {
          continue;
        }
        return lint_fail(diag, e, "%s `%s` of monitor `%.*s` names no "
                                  "declared variable",
                         what[l], name, m->len, m->name);
      }
    }
  }
  return RYSPEC_OK;
}
