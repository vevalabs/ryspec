/* The schema check: schemas/v0/ryspec.schema.json, by hand, reported as
 * RYSPEC_ERROR_SCHEMA at its first violation.
 *
 * The check reads the document as parsed, its expressions already prefix
 * form, so a rule position holds a prefix rule; a string beginning with `(`
 * anywhere else, which the parser leaves alone, is no dotted path and
 * fails. Each message names the key path of the value at fault, as
 * `monitors.default.outputs`, and the value's place in the document is the
 * diagnostic's line and column. Nothing under an `extras` table is read.
 *
 * Keep this in step with the schema: the corpus holds the two together,
 * every file the schema accepts passing here and every file declaring
 * #:expect-schema-error failing. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lint.h"

typedef struct schema {
  ryspec_diagnostic *diag;
  char path[512];
  size_t len;
} schema;

/* ---------------------------------------------------------------------------
 * Paths and failures. */

/* Append .key, or [i] for key NULL, to the path, returning its length
 * before, for pop(). */
static size_t push(schema *s, const char *key, int len, int i) {
  size_t before = s->len;
  size_t room = sizeof s->path - s->len;
  int n = key ? snprintf(s->path + s->len, room, "%s%.*s", s->len ? "." : "",
                         len, key)
              : snprintf(s->path + s->len, room, "[%d]", i);
  if (n > 0) {
    s->len = (size_t)n < room ? s->len + (size_t)n : sizeof s->path - 1;
  }
  return before;
}

static void pop(schema *s, size_t len) {
  s->len = len;
  s->path[len] = '\0';
}

static ryspec_status fail(schema *s, const toml_datum_t *at, const char *fmt,
                          ...) {
  char what[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(what, sizeof what, fmt, ap);
  va_end(ap);
  const char *path = s->len ? s->path : "the document";
  return lint_fail_status(s->diag, RYSPEC_ERROR_SCHEMA, at, "%s%s%s: %s",
                          s->len ? "`" : "", path, s->len ? "`" : "", what);
}

#define TRY(expr)                                                              \
  do {                                                                         \
    ryspec_status try_ = (expr);                                               \
    if (try_ != RYSPEC_OK) {                                                   \
      return try_;                                                             \
    }                                                                          \
  } while (0)

/* ---------------------------------------------------------------------------
 * Values. */

static const char *type_name(const toml_datum_t *d) {
  switch (d->type) {
  case TOML_STRING:
    return "a string";
  case TOML_INT64:
    return "an integer";
  case TOML_FP64:
    return "a float";
  case TOML_BOOLEAN:
    return "a boolean";
  case TOML_DATE:
    return "a date";
  case TOML_TIME:
    return "a time";
  case TOML_DATETIME:
  case TOML_DATETIMETZ:
    return "a datetime";
  case TOML_ARRAY:
    return "an array";
  case TOML_TABLE:
    return "a table";
  default:
    return "a value";
  }
}

static bool is_number(const toml_datum_t *d) {
  return d->type == TOML_INT64 || d->type == TOML_FP64;
}

/* JSON Schema's integer: a whole number, 1.0 among them. */
static bool is_integer(const toml_datum_t *d) {
  if (d->type == TOML_INT64) {
    return true;
  }
  /* Whole without libm: past 2^53 every double is whole. */
  double x = d->type == TOML_FP64 ? d->u.fp64 : 0.5;
  return isfinite(x) &&
         (x >= 9007199254740992.0 || x <= -9007199254740992.0 ||
          (double)(long long)x == x);
}

static double number(const toml_datum_t *d) {
  return d->type == TOML_INT64 ? (double)d->u.int64 : d->u.fp64;
}

static ryspec_status expect(schema *s, const toml_datum_t *d, bool ok,
                            const char *want) {
  return ok ? RYSPEC_OK
            : fail(s, d, "must be %s, not %s", want, type_name(d));
}

static bool is_identifier_bytes(const char *p, size_t len) {
  if (len == 0 || !(p[0] == '_' || (p[0] >= 'A' && p[0] <= 'Z') ||
                    (p[0] >= 'a' && p[0] <= 'z'))) {
    return false;
  }
  for (size_t i = 1; i < len; i++) {
    char c = p[i];
    if (!(c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9'))) {
      return false;
    }
  }
  return true;
}

static bool is_dotted_bytes(const char *p, size_t len) {
  size_t start = 0;
  for (size_t i = 0; i <= len; i++) {
    if (i == len || p[i] == '.') {
      if (!is_identifier_bytes(p + start, i - start)) {
        return false;
      }
      start = i + 1;
    }
  }
  return true;
}

static ryspec_status identifier(schema *s, const toml_datum_t *d) {
  TRY(expect(s, d, d->type == TOML_STRING, "a name"));
  if (!is_identifier_bytes(d->u.str.ptr, (size_t)d->u.str.len)) {
    return fail(s, d,
                "`%s` is not an identifier: a letter or `_`, then letters, "
                "digits or `_`",
                d->u.s);
  }
  return RYSPEC_OK;
}

static ryspec_status dotted_path(schema *s, const toml_datum_t *d) {
  TRY(expect(s, d, d->type == TOML_STRING, "a name"));
  if (!is_dotted_bytes(d->u.str.ptr, (size_t)d->u.str.len)) {
    return fail(s, d, "`%s` is not a name or a dotted path of names",
                d->u.s);
  }
  return RYSPEC_OK;
}

/* A string among the NULL-terminated names, listed as one_of says. */
static ryspec_status one_of(schema *s, const toml_datum_t *d,
                            const char *const *names, const char *list) {
  TRY(expect(s, d, d->type == TOML_STRING, "a string"));
  for (const char *const *n = names; *n; n++) {
    if (strcmp(d->u.s, *n) == 0) {
      return RYSPEC_OK;
    }
  }
  return fail(s, d, "`%s` is not one of %s", d->u.s, list);
}

static ryspec_status time_unit(schema *s, const toml_datum_t *d) {
  static const char *const units[] = {"ns", "us", "ms", "s",
                                      "min", "h", "d", NULL};
  return one_of(s, d, units, "ns, us, ms, s, min, h, d");
}

static ryspec_status string(schema *s, const toml_datum_t *d) {
  return expect(s, d, d->type == TOML_STRING, "a string");
}

static ryspec_status table(schema *s, const toml_datum_t *d) {
  return expect(s, d, d->type == TOML_TABLE, "a table");
}

/* ---------------------------------------------------------------------------
 * Tables. */

static const toml_datum_t *get(const toml_datum_t *t, const char *key) {
  return ryspec_toml_value_lookup(t, key, strlen(key));
}

static bool listed(const char *key, int len, const char *const *keys) {
  for (const char *const *k = keys; *k; k++) {
    if (strlen(*k) == (size_t)len && memcmp(*k, key, (size_t)len) == 0) {
      return true;
    }
  }
  return false;
}

/* Table t holds no key but those of the NULL-terminated keys. */
static ryspec_status only(schema *s, const toml_datum_t *t,
                          const char *const *keys) {
  for (int i = 0; i < t->u.tab.size; i++) {
    if (!listed(t->u.tab.key[i], t->u.tab.len[i], keys)) {
      return fail(s, &t->u.tab.value[i], "takes no key `%.*s`",
                  t->u.tab.len[i], t->u.tab.key[i]);
    }
  }
  return RYSPEC_OK;
}

static ryspec_status required(schema *s, const toml_datum_t *t,
                              const char *key) {
  return get(t, key) ? RYSPEC_OK
                     : fail(s, t, "lacks the required key `%s`", key);
}

/* Every key of table t is an identifier. */
static ryspec_status identifier_keys(schema *s, const toml_datum_t *t) {
  for (int i = 0; i < t->u.tab.size; i++) {
    if (!is_identifier_bytes(t->u.tab.key[i], (size_t)t->u.tab.len[i])) {
      return fail(s, &t->u.tab.value[i],
                  "key `%.*s` is not an identifier: a letter or `_`, then "
                  "letters, digits or `_`",
                  t->u.tab.len[i], t->u.tab.key[i]);
    }
  }
  return RYSPEC_OK;
}

typedef ryspec_status (*value_fn)(schema *s, const toml_datum_t *d);

/* Check the value of key in table t, when there is one, with fn. */
static ryspec_status at_key(schema *s, const toml_datum_t *t, const char *key,
                            value_fn fn) {
  const toml_datum_t *d = get(t, key);
  if (!d) {
    return RYSPEC_OK;
  }
  size_t before = push(s, key, (int)strlen(key), 0);
  ryspec_status st = fn(s, d);
  pop(s, before);
  return st;
}

/* A table keyed by identifiers, each value checked with fn. */
static ryspec_status keyed(schema *s, const toml_datum_t *t, value_fn fn) {
  TRY(table(s, t));
  TRY(identifier_keys(s, t));
  for (int i = 0; i < t->u.tab.size; i++) {
    size_t before = push(s, t->u.tab.key[i], t->u.tab.len[i], 0);
    ryspec_status st = fn(s, &t->u.tab.value[i]);
    pop(s, before);
    TRY(st);
  }
  return RYSPEC_OK;
}

/* An array of at least min items, each checked with fn, no two the same
 * string. */
static ryspec_status names(schema *s, const toml_datum_t *a, int min,
                           value_fn fn) {
  TRY(expect(s, a, a->type == TOML_ARRAY, "an array"));
  if (a->u.arr.size < min) {
    return fail(s, a, "must hold at least %d item%s", min, min == 1 ? "" : "s");
  }
  for (int i = 0; i < a->u.arr.size; i++) {
    const toml_datum_t *e = &a->u.arr.elem[i];
    size_t before = push(s, NULL, 0, i);
    ryspec_status st = fn(s, e);
    pop(s, before);
    TRY(st);
    for (int j = 0; j < i; j++) {
      const toml_datum_t *f = &a->u.arr.elem[j];
      if (f->u.str.len == e->u.str.len &&
          memcmp(f->u.str.ptr, e->u.str.ptr, (size_t)e->u.str.len) == 0) {
        return fail(s, e, "lists `%s` twice", e->u.s);
      }
    }
  }
  return RYSPEC_OK;
}

static ryspec_status free_form(schema *s, const toml_datum_t *d) {
  return table(s, d); /* extras: anything inside */
}

/* ---------------------------------------------------------------------------
 * Rules. */

static ryspec_status prefix_rule(schema *s, const toml_datum_t *d);

/* The i-th item of the rule array a, checked with fn. */
static ryspec_status item(schema *s, const toml_datum_t *a, int i,
                          value_fn fn) {
  size_t before = push(s, NULL, 0, i);
  ryspec_status st = fn(s, &a->u.arr.elem[i]);
  pop(s, before);
  return st;
}

static ryspec_status bound_end(schema *s, const toml_datum_t *d) {
  if (d->type == TOML_STRING) {
    return identifier(s, d);
  }
  TRY(expect(s, d, is_number(d), "a number or the name of a parameter"));
  return number(d) < 0 ? fail(s, d, "must not be negative") : RYSPEC_OK;
}

static ryspec_status bound(schema *s, const toml_datum_t *d) {
  static const char *const keys[] = {"min", "max", "time_unit", NULL};
  TRY(expect(s, d, d->type == TOML_TABLE, "a bound, a table"));
  TRY(only(s, d, keys));
  if (d->u.tab.size == 0) {
    return fail(s, d, "a bound gives `min`, `max` or both");
  }
  TRY(at_key(s, d, "min", bound_end));
  TRY(at_key(s, d, "max", bound_end));
  TRY(at_key(s, d, "time_unit", time_unit));
  if (get(d, "time_unit") && !get(d, "min") && !get(d, "max")) {
    return fail(s, d, "a `time_unit` comes with `min` or `max`");
  }
  return RYSPEC_OK;
}

/* A literal { value = ... }, its value a number, a string, or either. */
static ryspec_status literal(schema *s, const toml_datum_t *d, bool numbers,
                             bool strings) {
  static const char *const keys[] = {"value", NULL};
  const char *want = numbers && strings ? "a name or a literal, a table"
                     : numbers          ? "a name or a number literal, a table"
                                        : "a name or a string literal, a table";
  TRY(expect(s, d, d->type == TOML_TABLE, want));
  TRY(required(s, d, "value"));
  TRY(only(s, d, keys));
  const toml_datum_t *v = get(d, "value");
  size_t before = push(s, "value", 5, 0);
  ryspec_status st =
      expect(s, v, (numbers && is_number(v)) || (strings && v->type == TOML_STRING),
             numbers && strings ? "a number or a string"
             : numbers          ? "a number"
                                : "a string");
  pop(s, before);
  return st;
}

static ryspec_status number_operand(schema *s, const toml_datum_t *d) {
  return d->type == TOML_STRING ? identifier(s, d) : literal(s, d, true, false);
}

static ryspec_status string_operand(schema *s, const toml_datum_t *d) {
  return d->type == TOML_STRING ? identifier(s, d) : literal(s, d, false, true);
}

static ryspec_status any_operand(schema *s, const toml_datum_t *d) {
  return d->type == TOML_STRING ? identifier(s, d) : literal(s, d, true, true);
}

static ryspec_status quantified_variable(schema *s, const toml_datum_t *d) {
  static const char *const keys[] = {"qvar", NULL};
  TRY(expect(s, d, d->type == TOML_TABLE,
             "a quantified variable, { qvar = \"...\" }"));
  TRY(required(s, d, "qvar"));
  TRY(only(s, d, keys));
  return at_key(s, d, "qvar", identifier);
}

static ryspec_status qvar_name(schema *s, const toml_datum_t *d) {
  return identifier(s, d);
}

static ryspec_status qvars(schema *s, const toml_datum_t *d) {
  return names(s, d, 1, qvar_name);
}

static ryspec_status binding(schema *s, const toml_datum_t *d) {
  static const char *const keys[] = {"qvars", NULL};
  TRY(expect(s, d, d->type == TOML_TABLE, "a binding, { qvars = [...] }"));
  TRY(required(s, d, "qvars"));
  TRY(only(s, d, keys));
  return at_key(s, d, "qvars", qvars);
}

/* What follows each family's operator, for a message about its arity. */
static const char *takes(ryspec_rule_op op) {
  switch (op) {
  case RYSPEC_RULE_OP_NOT:
  case RYSPEC_RULE_OP_PREV:
  case RYSPEC_RULE_OP_NEXT:
    return "one operand";
  case RYSPEC_RULE_OP_AND:
  case RYSPEC_RULE_OP_OR:
  case RYSPEC_RULE_OP_XOR:
  case RYSPEC_RULE_OP_EQUIV:
  case RYSPEC_RULE_OP_IMPLIES:
    return "two or more operands";
  case RYSPEC_RULE_OP_ONCE:
  case RYSPEC_RULE_OP_HISTORICALLY:
  case RYSPEC_RULE_OP_EVENTUALLY:
  case RYSPEC_RULE_OP_ALWAYS:
    return "one operand and an optional bound";
  case RYSPEC_RULE_OP_SINCE:
  case RYSPEC_RULE_OP_UNTIL:
    return "two operands and an optional bound";
  case RYSPEC_RULE_OP_ASSIGN:
    return "a variable and a quantified variable";
  case RYSPEC_RULE_OP_FORALL:
  case RYSPEC_RULE_OP_EXISTS:
    return "a rule and a binding";
  default:
    return "two operands";
  }
}

static ryspec_status operator_rule(schema *s, const toml_datum_t *a) {
  const toml_datum_t *head = &a->u.arr.elem[0];
  ryspec_rule_op op;
  if (head->type != TOML_STRING || !ryspec_rule_op_from_name(head->u.s, &op)) {
    size_t before = push(s, NULL, 0, 0);
    ryspec_status st =
        head->type == TOML_STRING
            ? fail(s, head, "`%s` is not an operator", head->u.s)
            : fail(s, head, "a rule array begins with its operator, not %s",
                   type_name(head));
    pop(s, before);
    return st;
  }
  int n = a->u.arr.size - 1; /* after the operator */
  int lo, hi;
  switch (op) {
  case RYSPEC_RULE_OP_NOT:
  case RYSPEC_RULE_OP_PREV:
  case RYSPEC_RULE_OP_NEXT:
    lo = hi = 1;
    break;
  case RYSPEC_RULE_OP_AND:
  case RYSPEC_RULE_OP_OR:
  case RYSPEC_RULE_OP_XOR:
  case RYSPEC_RULE_OP_EQUIV:
  case RYSPEC_RULE_OP_IMPLIES:
    lo = 2;
    hi = n > 2 ? n : 2;
    break;
  case RYSPEC_RULE_OP_ONCE:
  case RYSPEC_RULE_OP_HISTORICALLY:
  case RYSPEC_RULE_OP_EVENTUALLY:
  case RYSPEC_RULE_OP_ALWAYS:
    lo = 1;
    hi = 2;
    break;
  case RYSPEC_RULE_OP_SINCE:
  case RYSPEC_RULE_OP_UNTIL:
    lo = 2;
    hi = 3;
    break;
  default: /* comparisons, assign and the quantifiers */
    lo = hi = 2;
    break;
  }
  if (n < lo || n > hi) {
    return fail(s, a, "`%s` takes %s, and has %d item%s after it", head->u.s,
                takes(op), n, n == 1 ? "" : "s");
  }
  switch (op) {
  case RYSPEC_RULE_OP_LT:
  case RYSPEC_RULE_OP_LE:
  case RYSPEC_RULE_OP_GT:
  case RYSPEC_RULE_OP_GE:
    TRY(item(s, a, 1, identifier));
    return item(s, a, 2, number_operand);
  case RYSPEC_RULE_OP_CONTAINS:
  case RYSPEC_RULE_OP_STARTSWITH:
  case RYSPEC_RULE_OP_ENDSWITH:
    TRY(item(s, a, 1, identifier));
    return item(s, a, 2, string_operand);
  case RYSPEC_RULE_OP_EQ:
  case RYSPEC_RULE_OP_NE:
    TRY(item(s, a, 1, identifier));
    return item(s, a, 2, any_operand);
  case RYSPEC_RULE_OP_ASSIGN:
    TRY(item(s, a, 1, identifier));
    return item(s, a, 2, quantified_variable);
  case RYSPEC_RULE_OP_FORALL:
  case RYSPEC_RULE_OP_EXISTS:
    TRY(item(s, a, 1, prefix_rule));
    return item(s, a, 2, binding);
  case RYSPEC_RULE_OP_ONCE:
  case RYSPEC_RULE_OP_HISTORICALLY:
  case RYSPEC_RULE_OP_EVENTUALLY:
  case RYSPEC_RULE_OP_ALWAYS:
    TRY(item(s, a, 1, prefix_rule));
    return n == 2 ? item(s, a, 2, bound) : RYSPEC_OK;
  case RYSPEC_RULE_OP_SINCE:
  case RYSPEC_RULE_OP_UNTIL:
    TRY(item(s, a, 1, prefix_rule));
    TRY(item(s, a, 2, prefix_rule));
    return n == 3 ? item(s, a, 3, bound) : RYSPEC_OK;
  default: /* not, prev, next and the n-ary operators */
    for (int i = 1; i <= n; i++) {
      TRY(item(s, a, i, prefix_rule));
    }
    return RYSPEC_OK;
  }
}

static ryspec_status prefix_rule(schema *s, const toml_datum_t *d) {
  if (d->type == TOML_STRING) {
    return dotted_path(s, d);
  }
  TRY(expect(s, d, d->type == TOML_ARRAY, "a rule: a name or an array"));
  if (d->u.arr.size == 0) {
    return fail(s, d, "a rule array holds at least its operator");
  }
  return operator_rule(s, d);
}

/* A rule position: an expression-form string, or a prefix rule. The
 * parser translates every expression at one, so a string left beginning
 * with `(` is an expression only in a document parsed some other way. */
static ryspec_status rule(schema *s, const toml_datum_t *d) {
  if (d->type == TOML_STRING && d->u.str.len > 0 && d->u.str.ptr[0] == '(') {
    const char *p = d->u.str.ptr;
    size_t len = (size_t)d->u.str.len;
    bool blank = true;
    for (size_t i = 1; i + 1 < len; i++) {
      blank = blank && strchr(" \t\r\n", p[i]) != NULL;
    }
    if (len < 3 || p[len - 1] != ')' || blank) {
      return fail(s, d, "an expression is a non-empty `(...)`");
    }
    return RYSPEC_OK;
  }
  return prefix_rule(s, d);
}

static ryspec_status rule_table(schema *s, const toml_datum_t *d) {
  return keyed(s, d, rule);
}

/* ---------------------------------------------------------------------------
 * Properties and namespaces. */

/* Whitespace to a regex's \s: ECMA-262's, which JSON Schema names, and
 * Python's, which jsonschema runs, U+001C to U+001F and U+0085 among
 * them; so a tag either refuses is refused here. */
static bool is_space(unsigned long c) {
  return (c >= 0x09 && c <= 0x0d) || (c >= 0x1c && c <= 0x20) || c == 0x85 ||
         c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) ||
         c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f ||
         c == 0x3000 || c == 0xfeff;
}

/* Whether the UTF-8 of len bytes at p, which the parser has checked,
 * holds whitespace. */
static bool has_whitespace(const char *p, size_t len) {
  const unsigned char *u = (const unsigned char *)p;
  for (size_t i = 0; i < len;) {
    unsigned long c = u[i];
    size_t n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    if (n > 1) {
      c &= 0x3fu >> (n - 1);
      for (size_t k = 1; k < n && i + k < len; k++) {
        c = (c << 6) | (u[i + k] & 0x3fu);
      }
    }
    if (is_space(c)) {
      return true;
    }
    i += n;
  }
  return false;
}

static ryspec_status tag(schema *s, const toml_datum_t *d) {
  TRY(string(s, d));
  if (d->u.str.len == 0 || has_whitespace(d->u.str.ptr, (size_t)d->u.str.len)) {
    return fail(s, d, "a tag is one word, without whitespace");
  }
  return RYSPEC_OK;
}

static ryspec_status tags(schema *s, const toml_datum_t *d) {
  return names(s, d, 0, tag);
}

static ryspec_status property(schema *s, const toml_datum_t *d) {
  static const char *const keys[] = {"title", "description", "message",
                                     "time_unit", "tags", "given", "check",
                                     "where", "extras", NULL};
  TRY(table(s, d));
  TRY(required(s, d, "check"));
  TRY(only(s, d, keys));
  TRY(at_key(s, d, "title", string));
  TRY(at_key(s, d, "description", string));
  TRY(at_key(s, d, "message", string));
  TRY(at_key(s, d, "time_unit", time_unit));
  TRY(at_key(s, d, "tags", tags));
  TRY(at_key(s, d, "given", rule));
  TRY(at_key(s, d, "check", rule));
  TRY(at_key(s, d, "where", rule_table));
  return at_key(s, d, "extras", free_form);
}

static ryspec_status property_table(schema *s, const toml_datum_t *d) {
  return keyed(s, d, property);
}

static ryspec_status namespace(schema *s, const toml_datum_t *d) {
  static const char *const leaf_keys[] = {"rules", "properties", "extras",
                                          NULL};
  TRY(table(s, d));
  TRY(identifier_keys(s, d));
  if (get(d, "rules") || get(d, "properties") || get(d, "extras")) {
    /* A leaf: logic, and no nested namespace. */
    TRY(only(s, d, leaf_keys));
    TRY(at_key(s, d, "rules", rule_table));
    TRY(at_key(s, d, "properties", property_table));
    return at_key(s, d, "extras", free_form);
  }
  return keyed(s, d, namespace);
}

static ryspec_status namespaces(schema *s, const toml_datum_t *d) {
  static const char *const own[] = {"rules", "properties", "extras", NULL};
  TRY(table(s, d));
  TRY(identifier_keys(s, d));
  for (int i = 0; i < d->u.tab.size; i++) {
    if (listed(d->u.tab.key[i], d->u.tab.len[i], own)) {
      return fail(s, &d->u.tab.value[i],
                  "takes no key `%.*s`: `namespace` is no namespace, and the "
                  "anonymous one's is at the root",
                  d->u.tab.len[i], d->u.tab.key[i]);
    }
  }
  return keyed(s, d, namespace);
}

/* ---------------------------------------------------------------------------
 * Variables and monitors. */

static ryspec_status boolean(schema *s, const toml_datum_t *d) {
  return expect(s, d, d->type == TOML_BOOLEAN, "a boolean");
}

static ryspec_status a_number(schema *s, const toml_datum_t *d) {
  return expect(s, d, is_number(d), "a number");
}

static ryspec_status text_format(schema *s, const toml_datum_t *d) {
  static const char *const formats[] = {"json", NULL};
  return one_of(s, d, formats, "json");
}

static ryspec_status binary_format(schema *s, const toml_datum_t *d) {
  static const char *const formats[] = {"flatbuffers", NULL};
  return one_of(s, d, formats, "flatbuffers");
}

static ryspec_status variable(schema *s, const toml_datum_t *d) {
  static const char *const types[] = {"bool", "number", "text", "binary",
                                      NULL};
  static const char *const bool_keys[] = {"type", "source", "title",
                                          "description", "extras",
                                          "initial_value", NULL};
  static const char *const number_keys[] = {
      "type", "source", "title", "description", "extras", "initial_value",
      "min", "max", NULL};
  static const char *const format_keys[] = {"type",        "source", "title",
                                            "description", "extras", "format",
                                            NULL};
  TRY(table(s, d));
  const toml_datum_t *type = get(d, "type");
  if (type) {
    size_t before = push(s, "type", 4, 0);
    ryspec_status st = one_of(s, type, types, "bool, number, text, binary");
    pop(s, before);
    TRY(st);
  }
  const char *t = type ? type->u.s : "bool";
  TRY(at_key(s, d, "source", dotted_path));
  TRY(at_key(s, d, "title", string));
  TRY(at_key(s, d, "description", string));
  TRY(at_key(s, d, "extras", free_form));
  if (strcmp(t, "bool") == 0) {
    TRY(only(s, d, bool_keys));
    return at_key(s, d, "initial_value", boolean);
  }
  if (strcmp(t, "number") == 0) {
    TRY(only(s, d, number_keys));
    TRY(at_key(s, d, "initial_value", a_number));
    TRY(at_key(s, d, "min", a_number));
    return at_key(s, d, "max", a_number);
  }
  TRY(only(s, d, format_keys));
  return at_key(s, d, "format",
                strcmp(t, "text") == 0 ? text_format : binary_format);
}

static ryspec_status variables(schema *s, const toml_datum_t *d) {
  return keyed(s, d, variable);
}

static ryspec_status at_least_one(schema *s, const toml_datum_t *d) {
  TRY(expect(s, d, is_integer(d), "an integer"));
  return number(d) < 1 ? fail(s, d, "must be at least 1") : RYSPEC_OK;
}

static ryspec_status positive(schema *s, const toml_datum_t *d) {
  TRY(a_number(s, d));
  return number(d) <= 0 ? fail(s, d, "must be greater than 0") : RYSPEC_OK;
}

static ryspec_status runtime(schema *s, const toml_datum_t *d) {
  static const char *const keys[] = {"allocation_size", "buffer_size",
                                     "base_period", "time_unit", NULL};
  TRY(table(s, d));
  TRY(only(s, d, keys));
  TRY(at_key(s, d, "allocation_size", at_least_one));
  TRY(at_key(s, d, "buffer_size", at_least_one));
  TRY(at_key(s, d, "base_period", positive));
  TRY(at_key(s, d, "time_unit", time_unit));
  if (get(d, "base_period") && !get(d, "time_unit")) {
    return fail(s, get(d, "base_period"),
                "a `base_period` is in the monitor's `time_unit`, which is "
                "missing");
  }
  return RYSPEC_OK;
}

static ryspec_status identifiers(schema *s, const toml_datum_t *d) {
  return names(s, d, 0, identifier);
}

static ryspec_status outputs(schema *s, const toml_datum_t *d) {
  return names(s, d, 1, dotted_path);
}

static ryspec_status monitor(schema *s, const toml_datum_t *d) {
  static const char *const keys[] = {"runtime", "inputs", "outputs",
                                     "parameters", NULL};
  TRY(table(s, d));
  TRY(required(s, d, "outputs"));
  TRY(only(s, d, keys));
  TRY(at_key(s, d, "runtime", runtime));
  TRY(at_key(s, d, "inputs", identifiers));
  TRY(at_key(s, d, "outputs", outputs));
  return at_key(s, d, "parameters", identifiers);
}

static ryspec_status monitors(schema *s, const toml_datum_t *d) {
  return keyed(s, d, monitor);
}

/* ---------------------------------------------------------------------------
 * The document. */

static ryspec_status version(schema *s, const toml_datum_t *d) {
  TRY(string(s, d));
  return strcmp(d->u.s, "0") == 0 ? RYSPEC_OK
                                  : fail(s, d, "must be \"0\", not \"%s\"",
                                         d->u.s);
}

static ryspec_status meta(schema *s, const toml_datum_t *d) {
  static const char *const keys[] = {"title", "description", NULL};
  TRY(table(s, d));
  TRY(only(s, d, keys));
  TRY(at_key(s, d, "title", string));
  return at_key(s, d, "description", string);
}

ryspec_status lint_check_schema(const lint_index *idx,
                                ryspec_diagnostic *diag) {
  static const char *const keys[] = {"version",    "meta",  "variables",
                                     "monitors",   "rules", "properties",
                                     "namespace",  "extras", NULL};
  schema sc = {.diag = diag};
  schema *s = &sc;
  const toml_datum_t *root = idx->namespaces[0].table;
  TRY(required(s, root, "version"));
  TRY(only(s, root, keys));
  TRY(at_key(s, root, "version", version));
  TRY(at_key(s, root, "meta", meta));
  TRY(at_key(s, root, "variables", variables));
  TRY(at_key(s, root, "monitors", monitors));
  TRY(at_key(s, root, "rules", rule_table));
  TRY(at_key(s, root, "properties", property_table));
  TRY(at_key(s, root, "namespace", namespaces));
  return at_key(s, root, "extras", free_form);
}
