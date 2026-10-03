/* The checks of numbers: Rules 7 and 23. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lint.h"

static const toml_datum_t *get(const toml_datum_t *t, const char *key,
                               size_t len) {
  return ryspec_toml_value_lookup(t, key, len);
}

/* The number d holds in *out, returning whether it is one. */
static bool number(const toml_datum_t *d, double *out) {
  if (!d) {
    return false;
  }
  if (d->type == TOML_INT64) {
    *out = (double)d->u.int64;
    return true;
  }
  if (d->type == TOML_FP64) {
    *out = d->u.fp64;
    return true;
  }
  return false;
}

static bool is_nan(const toml_datum_t *d) {
  return d && d->type == TOML_FP64 && isnan(d->u.fp64);
}

/* Fail at at when table t's min is greater than its max, both numbers. */
static ryspec_status ordered(const toml_datum_t *t, const toml_datum_t *at,
                             const char *what, ryspec_diagnostic *diag) {
  const toml_datum_t *min = get(t, "min", 3), *max = get(t, "max", 3);
  double lo, hi;
  if (!number(min, &lo) || !number(max, &hi) || !(lo > hi)) {
    return RYSPEC_OK;
  }
  char lo_text[32], hi_text[32];
  lint_number_text(min, lo_text, sizeof lo_text);
  lint_number_text(max, hi_text, sizeof hi_text);
  return lint_fail(diag, at, "%s min (%s) is greater than max (%s)", what,
                   lo_text, hi_text);
}

static ryspec_status bound_ordered(const lint_node *n, void *ctx,
                                   ryspec_diagnostic *diag) {
  (void)ctx;
  if (n->kind != LINT_BOUND) {
    return RYSPEC_OK;
  }
  return ordered(n->datum, n->at, "the bound's", diag);
}

/* Rule 7: min is not greater than max, on a number variable or on a metric
 * bound. */
ryspec_status lint_check_min_le_max(const lint_index *idx,
                                    ryspec_diagnostic *diag) {
  for (size_t i = 0; i < idx->n_variables; i++) {
    const lint_variable *v = &idx->variables[i];
    if (v->type != LINT_NUMBER) {
      continue;
    }
    char what[128];
    snprintf(what, sizeof what, "`%.*s`'s", v->len, v->name);
    const toml_datum_t *min = get(v->decl, "min", 3);
    ryspec_status s = ordered(v->decl, min ? min : v->decl, what, diag);
    if (s != RYSPEC_OK) {
      return s;
    }
  }
  return lint_walk_all(idx, bound_ordered, NULL, diag);
}

static ryspec_status no_nan_in_rule(const lint_node *n, void *ctx,
                                    ryspec_diagnostic *diag) {
  (void)ctx;
  if (n->kind == LINT_LITERAL && is_nan(get(n->datum, "value", 5))) {
    return lint_fail(diag, n->at, "a literal is nan, and no number is");
  }
  if (n->kind == LINT_BOUND) {
    static const char *ends[] = {"min", "max"};
    for (int i = 0; i < 2; i++) {
      if (is_nan(get(n->datum, ends[i], 3))) {
        return lint_fail(diag, n->at, "the bound's %s is nan, and no number is",
                         ends[i]);
      }
    }
  }
  return RYSPEC_OK;
}

/* Rule 23: no number is nan: a variable's initial_value, min or max, a
 * monitor's base_period, a literal or a bound's end. */
ryspec_status lint_check_no_nan(const lint_index *idx,
                                ryspec_diagnostic *diag) {
  static const char *keys[] = {"initial_value", "min", "max"};
  for (size_t i = 0; i < idx->n_variables; i++) {
    const lint_variable *v = &idx->variables[i];
    for (int k = 0; k < 3; k++) {
      const toml_datum_t *d = get(v->decl, keys[k], strlen(keys[k]));
      if (is_nan(d)) {
        return lint_fail(diag, d, "`%.*s`'s %s is nan, and no number is",
                         v->len, v->name, keys[k]);
      }
    }
  }
  for (size_t i = 0; i < idx->n_monitors; i++) {
    const lint_monitor *m = &idx->monitors[i];
    const toml_datum_t *period =
        get(get(m->table, "runtime", 7), "base_period", 11);
    if (is_nan(period)) {
      return lint_fail(diag, period,
                       "monitor `%.*s`'s base_period is nan, and no number is",
                       m->len, m->name);
    }
  }
  return lint_walk_all(idx, no_nan_in_rule, NULL, diag);
}

/* Rule 8: a multi-segment source path has a declared head that carries a
 * format. */
ryspec_status lint_check_source_head(const lint_index *idx,
                                     ryspec_diagnostic *diag) {
  for (size_t i = 0; i < idx->n_variables; i++) {
    const lint_variable *v = &idx->variables[i];
    const toml_datum_t *source = get(v->decl, "source", 6);
    if (!source || source->type != TOML_STRING) {
      continue;
    }
    const char *path = source->u.s;
    const char *dot = strchr(path, '.');
    if (!dot) {
      continue;
    }
    int head_len = (int)(dot - path);
    const lint_variable *head = lint_variable_find(idx, path, (size_t)head_len);
    if (!head) {
      return lint_fail(diag, source,
                       "`%.*s`'s source `%s` reads from `%.*s`, which no "
                       "variable declares",
                       v->len, v->name, path, head_len, path);
    }
    if (!get(head->decl, "format", 6)) {
      return lint_fail(diag, source,
                       "`%.*s`'s source `%s` reads a field of `%.*s`, which "
                       "declares no format",
                       v->len, v->name, path, head_len, path);
    }
  }
  return RYSPEC_OK;
}
