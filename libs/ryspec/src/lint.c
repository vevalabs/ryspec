/* The linter: the index, the walker, and the checks dispatched by rule. */
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "lint.h"

/* ---------------------------------------------------------------------------
 * The index. */

/* What building the index carries: the allocator, and each array's
 * capacity. */
typedef struct builder {
  lint_index *idx;
  toml_option_t opt;
  size_t cap_namespaces, cap_properties, cap_rules, cap_variables,
      cap_monitors;
} builder;

/* Make room for one more item in *items, of n items of size bytes. */
static bool grow(builder *b, void **items, size_t n, size_t *cap,
                 size_t size) {
  if (n < *cap) {
    return true;
  }
  size_t want = *cap ? *cap * 2 : 8;
  void *p = b->opt.mem_realloc(*items, want * size);
  if (!p) {
    return false;
  }
  *items = p;
  *cap = want;
  return true;
}

#define PUSH(b, array, item)                                                   \
  (grow((b), (void **)&(b)->idx->array, (b)->idx->n_##array,                   \
        &(b)->cap_##array, sizeof *(b)->idx->array) &&                         \
   ((b)->idx->array[(b)->idx->n_##array++] = (item), true))

static const toml_datum_t *get(const toml_datum_t *t, const char *key) {
  return ryspec_toml_value_lookup(t, key, strlen(key));
}

static bool is_table(const toml_datum_t *d) {
  return d && d->type == TOML_TABLE;
}

static bool key_is(const toml_datum_t *t, int i, const char *key) {
  return (size_t)t->u.tab.len[i] == strlen(key) &&
         memcmp(t->u.tab.key[i], key, (size_t)t->u.tab.len[i]) == 0;
}

/* The rules of table t, each a rule position of kind. */
static bool index_rules(builder *b, const toml_datum_t *t, lint_rule_kind kind,
                        int ns, int property) {
  if (!is_table(t)) {
    return true;
  }
  for (int i = 0; i < t->u.tab.size; i++) {
    lint_rule r = {kind, ns, property, t->u.tab.key[i], t->u.tab.len[i],
                   &t->u.tab.value[i]};
    if (!PUSH(b, rules, r)) {
      return false;
    }
  }
  return true;
}

static bool index_position(builder *b, const toml_datum_t *v,
                           lint_rule_kind kind, int ns, int property) {
  if (!v) {
    return true;
  }
  lint_rule r = {kind, ns, property, NULL, 0, v};
  return PUSH(b, rules, r);
}

/* The rules and properties of the namespace ns. */
static bool index_logic(builder *b, int ns) {
  const toml_datum_t *t = b->idx->namespaces[ns].table;
  if (!index_rules(b, get(t, "rules"), LINT_RULE, ns, -1)) {
    return false;
  }
  const toml_datum_t *properties = get(t, "properties");
  if (!is_table(properties)) {
    return true;
  }
  for (int i = 0; i < properties->u.tab.size; i++) {
    const toml_datum_t *p = &properties->u.tab.value[i];
    lint_property prop = {ns, properties->u.tab.key[i],
                          properties->u.tab.len[i], p};
    if (!PUSH(b, properties, prop)) {
      return false;
    }
    int at = (int)b->idx->n_properties - 1;
    if (!index_position(b, get(p, "given"), LINT_GIVEN, ns, at) ||
        !index_position(b, get(p, "check"), LINT_CHECK, ns, at) ||
        !index_rules(b, get(p, "where"), LINT_PRIVATE_RULE, ns, at)) {
      return false;
    }
  }
  return true;
}

/* The named namespaces under t, a table of them, children of parent. */
static bool index_namespaces(builder *b, const toml_datum_t *t, int parent) {
  if (!is_table(t)) {
    return true;
  }
  for (int i = 0; i < t->u.tab.size; i++) {
    const toml_datum_t *v = &t->u.tab.value[i];
    if (!is_table(v) || key_is(t, i, "rules") || key_is(t, i, "properties") ||
        key_is(t, i, "extras")) {
      continue;
    }
    lint_namespace ns = {parent, t->u.tab.key[i], t->u.tab.len[i], v};
    if (!PUSH(b, namespaces, ns)) {
      return false;
    }
    int at = (int)b->idx->n_namespaces - 1;
    if (!index_logic(b, at) || !index_namespaces(b, v, at)) {
      return false;
    }
  }
  return true;
}

static lint_type type_of(const toml_datum_t *decl) {
  const toml_datum_t *type = get(decl, "type");
  if (!type || type->type != TOML_STRING) {
    return LINT_BOOL;
  }
  if (strcmp(type->u.s, "number") == 0) {
    return LINT_NUMBER;
  }
  if (strcmp(type->u.s, "text") == 0) {
    return LINT_TEXT;
  }
  if (strcmp(type->u.s, "binary") == 0) {
    return LINT_BINARY;
  }
  return LINT_BOOL;
}

static bool index_variables(builder *b, const toml_datum_t *t) {
  if (!is_table(t)) {
    return true;
  }
  for (int i = 0; i < t->u.tab.size; i++) {
    const toml_datum_t *decl = &t->u.tab.value[i];
    if (!is_table(decl)) {
      continue;
    }
    lint_variable v = {t->u.tab.key[i], t->u.tab.len[i], type_of(decl), decl};
    if (!PUSH(b, variables, v)) {
      return false;
    }
  }
  return true;
}

static const toml_datum_t *array_at(const toml_datum_t *t, const char *key) {
  const toml_datum_t *v = get(t, key);
  return v && v->type == TOML_ARRAY ? v : NULL;
}

static bool index_monitors(builder *b, const toml_datum_t *t) {
  if (!is_table(t)) {
    return true;
  }
  for (int i = 0; i < t->u.tab.size; i++) {
    const toml_datum_t *m = &t->u.tab.value[i];
    if (!is_table(m)) {
      continue;
    }
    lint_monitor mon = {t->u.tab.key[i],          t->u.tab.len[i],
                        m,                        array_at(m, "inputs"),
                        array_at(m, "parameters"), array_at(m, "outputs")};
    if (!PUSH(b, monitors, mon)) {
      return false;
    }
  }
  return true;
}

ryspec_status lint_index_build(const ryspec_toml_doc *doc, lint_index *idx,
                               ryspec_diagnostic *diag) {
  *idx = (lint_index){0};
  builder b = {.idx = idx, .opt = ryspec_toml_options()};
  const toml_datum_t *root = &doc->result.toptab;
  lint_namespace anonymous = {-1, NULL, 0, root};
  if (!PUSH(&b, namespaces, anonymous) || !index_logic(&b, 0) ||
      !index_namespaces(&b, get(root, "namespace"), 0) ||
      !index_variables(&b, get(root, "variables")) ||
      !index_monitors(&b, get(root, "monitors"))) {
    ryspec_diagnose(diag, RYSPEC_ERROR_MEMORY, 0, 0, "out of memory");
    return RYSPEC_ERROR_MEMORY;
  }
  return RYSPEC_OK;
}

void lint_index_free(lint_index *idx) {
  void (*mem_free)(void *) = ryspec_toml_options().mem_free;
  mem_free(idx->namespaces);
  mem_free(idx->properties);
  mem_free(idx->rules);
  mem_free(idx->variables);
  mem_free(idx->monitors);
  *idx = (lint_index){0};
}

const lint_variable *lint_variable_find(const lint_index *idx,
                                        const char *name, size_t len) {
  for (size_t i = 0; i < idx->n_variables; i++) {
    const lint_variable *v = &idx->variables[i];
    if ((size_t)v->len == len && memcmp(v->name, name, len) == 0) {
      return v;
    }
  }
  return NULL;
}

bool lint_array_holds(const toml_datum_t *a, const char *name, size_t len) {
  for (int i = 0; a && i < a->u.arr.size; i++) {
    const toml_datum_t *e = &a->u.arr.elem[i];
    if (e->type == TOML_STRING && (size_t)e->u.str.len == len &&
        memcmp(e->u.str.ptr, name, len) == 0) {
      return true;
    }
  }
  return false;
}

bool lint_monitor_lists(const lint_monitor *m, const char *name, size_t len) {
  return lint_array_holds(m->inputs, name, len) ||
         lint_array_holds(m->parameters, name, len) ||
         lint_array_holds(m->outputs, name, len);
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
                          const lint_rule *rule, lint_visit fn, void *ctx,
                          ryspec_diagnostic *diag) {
  lint_node n = {.datum = v, .parent = parent, .rule = rule};
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
  n.known = head->type == TOML_STRING && ryspec_rule_op_from_name(head->u.s,
                                                                  &n.op);
  ryspec_status s = fn(&n, ctx, diag);
  for (int i = 1; s == RYSPEC_OK && i < v->u.arr.size; i++) {
    s = walk(&v->u.arr.elem[i], &n, rule, fn, ctx, diag);
  }
  return s;
}

ryspec_status lint_walk(const toml_datum_t *value, lint_visit fn, void *ctx,
                        ryspec_diagnostic *diag) {
  return walk(value, NULL, NULL, fn, ctx, diag);
}

ryspec_status lint_walk_rule(const lint_rule *r, lint_visit fn, void *ctx,
                             ryspec_diagnostic *diag) {
  return walk(r->value, NULL, r, fn, ctx, diag);
}

ryspec_status lint_walk_all(const lint_index *idx, lint_visit fn, void *ctx,
                            ryspec_diagnostic *diag) {
  ryspec_status s = RYSPEC_OK;
  for (size_t i = 0; s == RYSPEC_OK && i < idx->n_rules; i++) {
    s = lint_walk_rule(&idx->rules[i], fn, ctx, diag);
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
 * Names. */

static bool same(const char *a, size_t a_len, const char *b, int b_len) {
  return a_len == (size_t)b_len && memcmp(a, b, a_len) == 0;
}

/* Whether the namespace ns is the one at the dotted path of len bytes,
 * the root's being empty. */
static bool namespace_is(const lint_index *idx, int ns, const char *path,
                         size_t len) {
  if (ns <= 0) {
    return ns == 0 && len == 0;
  }
  const lint_namespace *n = &idx->namespaces[ns];
  size_t name_len = (size_t)n->len;
  if (len < name_len ||
      !same(path + len - name_len, name_len, n->name, n->len)) {
    return false;
  }
  if (len == name_len) {
    return n->parent == 0;
  }
  return path[len - name_len - 1] == '.' &&
         namespace_is(idx, n->parent, path, len - name_len - 1);
}

/* One more thing a name answers to: the first, or past it, ambiguity. */
static void found(lint_target *t, lint_target_kind kind, size_t index) {
  if (t->kind == LINT_TO_DEDUCED || t->kind == LINT_TO_NOTHING) {
    *t = (lint_target){kind, index};
  } else {
    t->kind = LINT_TO_AMBIGUOUS;
  }
}

static lint_target resolve_path(const lint_index *idx, const char *name,
                                size_t len) {
  lint_target t = {LINT_TO_NOTHING, 0};
  const char *dot = name + len;
  while (dot > name && dot[-1] != '.') {
    dot--;
  }
  size_t prefix = (size_t)(dot - name) - 1;
  size_t last = len - prefix - 1;
  for (size_t i = 0; i < idx->n_rules; i++) {
    const lint_rule *r = &idx->rules[i];
    if (r->kind == LINT_RULE && same(dot, last, r->name, r->len) &&
        r->ns > 0 && namespace_is(idx, r->ns, name, prefix)) {
      found(&t, LINT_TO_RULE, i);
    }
  }
  for (size_t i = 0; i < idx->n_properties; i++) {
    const lint_property *p = &idx->properties[i];
    if (same(dot, last, p->name, p->len) && p->ns > 0 &&
        namespace_is(idx, p->ns, name, prefix)) {
      found(&t, LINT_TO_PROPERTY, i);
    }
  }
  for (size_t i = 1; t.kind == LINT_TO_NOTHING && i < idx->n_namespaces;
       i++) {
    if (namespace_is(idx, (int)i, name, len)) {
      t = (lint_target){LINT_TO_NAMESPACE, i};
    }
  }
  return t;
}

lint_target lint_resolve(const lint_index *idx, const lint_rule *at,
                         const char *name, size_t len) {
  if (memchr(name, '.', len)) {
    return resolve_path(idx, name, len);
  }
  int ns = at ? at->ns : 0;
  int property = at ? at->property : -1;
  lint_target t = {LINT_TO_DEDUCED, 0};
  for (size_t i = 0; i < idx->n_rules; i++) {
    const lint_rule *r = &idx->rules[i];
    bool visible = (r->kind == LINT_RULE && r->ns == ns) ||
                   (r->kind == LINT_PRIVATE_RULE && property >= 0 &&
                    r->property == property);
    if (visible && same(name, len, r->name, r->len)) {
      found(&t, LINT_TO_RULE, i);
    }
  }
  for (size_t i = 0; i < idx->n_properties; i++) {
    const lint_property *p = &idx->properties[i];
    if (p->ns == ns && same(name, len, p->name, p->len)) {
      found(&t, LINT_TO_PROPERTY, i);
    }
  }
  for (size_t i = 0; i < idx->n_variables; i++) {
    if (same(name, len, idx->variables[i].name, idx->variables[i].len)) {
      found(&t, LINT_TO_VARIABLE, i);
    }
  }
  for (size_t i = 1; ns == 0 && i < idx->n_namespaces; i++) {
    const lint_namespace *n = &idx->namespaces[i];
    if (n->parent == 0 && same(name, len, n->name, n->len)) {
      found(&t, LINT_TO_NAMESPACE, i);
    }
  }
  return t;
}

bool lint_target_type(const lint_index *idx, lint_target t, lint_type *out) {
  switch (t.kind) {
  case LINT_TO_VARIABLE:
    *out = idx->variables[t.index].type;
    return true;
  case LINT_TO_RULE:
  case LINT_TO_PROPERTY:
    *out = LINT_BOOL;
    return true;
  default:
    return false;
  }
}

const char *lint_type_name(lint_type t) {
  static const char *names[] = {"bool", "number", "text", "binary"};
  return names[t];
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
    RYSPEC_LINT_MONITOR_LIST_OVERLAP,   RYSPEC_LINT_MONITOR_ENTRY_RESOLVES,
    RYSPEC_LINT_INITIAL_VALUE_LISTED,   RYSPEC_LINT_PARAMETER_INITIAL,
    RYSPEC_LINT_MIN_LE_MAX,             RYSPEC_LINT_SOURCE_HEAD,
    RYSPEC_LINT_NO_TEXT_AS_TRUTH,       RYSPEC_LINT_ONE_SOURCE_OF_VALUE,
    RYSPEC_LINT_ONE_CONE,               RYSPEC_LINT_NO_CYCLE,
    RYSPEC_LINT_QVAR_BOUND,             RYSPEC_LINT_QVAR_NO_REBIND,
    RYSPEC_LINT_QVAR_NO_SHADOW,         RYSPEC_LINT_QVAR_USED,
    RYSPEC_LINT_PARAMETER_NO_SOURCE,    RYSPEC_LINT_BOUND_NAME_NUMBER,
    RYSPEC_LINT_COMPARISON_TYPES,       RYSPEC_LINT_NO_NAMESPACE_REFERENCE,
    RYSPEC_LINT_NO_NAN,
};

const ryspec_lint_rule *ryspec_lint_rules(size_t *count) {
  *count = N_CHECKS;
  return rules;
}

/* Run the checks [first, last) over doc, stopping at the first violation. */
static ryspec_status run(const ryspec_toml_doc *doc, size_t first, size_t last,
                         ryspec_diagnostic *diag) {
  lint_index idx;
  ryspec_status s = lint_index_build(doc, &idx, diag);
  for (size_t i = first; s == RYSPEC_OK && i < last; i++) {
    s = checks[i].fn(&idx, diag);
  }
  lint_index_free(&idx);
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
