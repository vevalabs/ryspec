/* The checks: the schema's, check 0, then one per rule of SPEC.md's "What
 * the schema cannot check", each a ryspec_lint_check_*() that lint.c dispatches
 * by number. A section holds the checks of what they read, in the order
 * ryspec_toml_lint() runs them. */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lint.h"

/* ---------------------------------------------------------------------------
 * What the checks share. */

static const toml_datum_t* get(const toml_datum_t* t, const char* key)
{
  return ryspec_toml_value_lookup(t, key, strlen(key));
}

/* A monitor or a variable: a table of the root's `monitors` or
 * `variables`, its key, for `%.*s` len and name, and its declaration. */
typedef struct decl {
  int len;
  const char* name;
  const toml_datum_t* value;
} decl;

/* The first table t holds from the *i-th entry on, in *out, *i set to its
 * entry, returning whether there is one: so
 *   for (int i = 0; next_decl(t, &i, &d); i++)
 * visits every table t holds, passing over values of another shape. */
static bool next_decl(const toml_datum_t* t, int* i, decl* out)
{
  for(; t && t->type == TOML_TABLE && *i < t->u.tab.size; (*i)++) {
    const toml_datum_t* v = &t->u.tab.value[*i];
    if(v->type == TOML_TABLE) {
      *out = (decl){t->u.tab.len[*i], t->u.tab.key[*i], v};
      return true;
    }
  }
  return false;
}

static const toml_datum_t* monitors_of(const ryspec_toml_doc* doc)
{
  return get(ryspec_toml_doc_root(doc), "monitors");
}

static const toml_datum_t* variables_of(const ryspec_toml_doc* doc)
{
  return get(ryspec_toml_doc_root(doc), "variables");
}

/* d, where it has a place in the document, or else fallback. */
static const toml_datum_t* placed(
  const toml_datum_t* d, const toml_datum_t* fallback)
{
  return d->lineno ? d : fallback;
}

/* ===========================================================================
 * The schema check: schemas/v0/ryspec.schema.json, by hand, reported as
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

typedef struct schema {
  ryspec_diag* diag;
  char path[512];
  size_t len;
} schema;

/* ---------------------------------------------------------------------------
 * Paths and failures. */

/* Append .key, or [i] for key NULL, to the path, returning its length
 * before, for pop(). */
static size_t push(schema* s, const char* key, int len, int i)
{
  size_t before = s->len;
  size_t room = sizeof s->path - s->len;
  int n = key ? snprintf(
                  s->path + s->len, room, "%s%.*s", s->len ? "." : "", len, key)
              : snprintf(s->path + s->len, room, "[%d]", i);
  if(n > 0) {
    s->len = (size_t)n < room ? s->len + (size_t)n : sizeof s->path - 1;
  }
  return before;
}

static void pop(schema* s, size_t len)
{
  s->len = len;
  s->path[len] = '\0';
}

static int fail(schema* s, const toml_datum_t* at, const char* fmt, ...)
{
  char what[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(what, sizeof what, fmt, ap);
  va_end(ap);
  const char* path = s->len ? s->path : "the document";
  return ryspec_lint_fail_status(
    s->diag,
    RYSPEC_ERROR_SCHEMA,
    at,
    "%s%s%s: %s",
    s->len ? "`" : "",
    path,
    s->len ? "`" : "",
    what);
}

#define TRY(expr)           \
  do {                      \
    int try_ = (expr);      \
    if(try_ != RYSPEC_OK) { \
      return try_;          \
    }                       \
  } while(0)

/* ---------------------------------------------------------------------------
 * Values. */

static const char* type_name(const toml_datum_t* d)
{
  switch(d->type) {
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

static bool is_number(const toml_datum_t* d)
{
  return d->type == TOML_INT64 || d->type == TOML_FP64;
}

/* JSON Schema's integer: a whole number, 1.0 among them. */
static bool is_integer(const toml_datum_t* d)
{
  if(d->type == TOML_INT64) {
    return true;
  }
  /* Whole without libm: past 2^53 every double is whole. */
  double x = d->type == TOML_FP64 ? d->u.fp64 : 0.5;
  return isfinite(x) && (x >= 9007199254740992.0 || x <= -9007199254740992.0 ||
                         (double)(long long)x == x);
}

static double number(const toml_datum_t* d)
{
  return d->type == TOML_INT64 ? (double)d->u.int64 : d->u.fp64;
}

static int expect(schema* s, const toml_datum_t* d, bool ok, const char* want)
{
  return ok ? RYSPEC_OK : fail(s, d, "must be %s, not %s", want, type_name(d));
}

static bool is_identifier_bytes(const char* p, size_t len)
{
  if(
    len == 0 || !(p[0] == '_' || (p[0] >= 'A' && p[0] <= 'Z') ||
                  (p[0] >= 'a' && p[0] <= 'z'))) {
    return false;
  }
  for(size_t i = 1; i < len; i++) {
    char c = p[i];
    if(!(c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9'))) {
      return false;
    }
  }
  return true;
}

static bool is_dotted_bytes(const char* p, size_t len)
{
  size_t start = 0;
  for(size_t i = 0; i <= len; i++) {
    if(i == len || p[i] == '.') {
      if(!is_identifier_bytes(p + start, i - start)) {
        return false;
      }
      start = i + 1;
    }
  }
  return true;
}

static int identifier(schema* s, const toml_datum_t* d)
{
  TRY(expect(s, d, d->type == TOML_STRING, "a name"));
  if(!is_identifier_bytes(d->u.str.ptr, (size_t)d->u.str.len)) {
    return fail(
      s,
      d,
      "`%s` is not an identifier: a letter or `_`, then letters, "
      "digits or `_`",
      d->u.s);
  }
  return RYSPEC_OK;
}

static int dotted_path(schema* s, const toml_datum_t* d)
{
  TRY(expect(s, d, d->type == TOML_STRING, "a name"));
  if(!is_dotted_bytes(d->u.str.ptr, (size_t)d->u.str.len)) {
    return fail(s, d, "`%s` is not a name or a dotted path of names", d->u.s);
  }
  return RYSPEC_OK;
}

/* A string among the NULL-terminated names, listed as one_of says. */
static int one_of(
  schema* s, const toml_datum_t* d, const char* const* names, const char* list)
{
  TRY(expect(s, d, d->type == TOML_STRING, "a string"));
  for(const char* const* n = names; *n; n++) {
    if(strcmp(d->u.s, *n) == 0) {
      return RYSPEC_OK;
    }
  }
  return fail(s, d, "`%s` is not one of %s", d->u.s, list);
}

static int time_unit(schema* s, const toml_datum_t* d)
{
  static const char* const units[] = {
    "ns", "us", "ms", "s", "min", "h", "d", NULL};
  return one_of(s, d, units, "ns, us, ms, s, min, h, d");
}

static int string(schema* s, const toml_datum_t* d)
{
  return expect(s, d, d->type == TOML_STRING, "a string");
}

static int table(schema* s, const toml_datum_t* d)
{
  return expect(s, d, d->type == TOML_TABLE, "a table");
}

/* ---------------------------------------------------------------------------
 * Tables. */

static bool listed(const char* key, int len, const char* const* keys)
{
  for(const char* const* k = keys; *k; k++) {
    if(strlen(*k) == (size_t)len && memcmp(*k, key, (size_t)len) == 0) {
      return true;
    }
  }
  return false;
}

/* Table t holds no key but those of the NULL-terminated keys. */
static int only(schema* s, const toml_datum_t* t, const char* const* keys)
{
  for(int i = 0; i < t->u.tab.size; i++) {
    if(!listed(t->u.tab.key[i], t->u.tab.len[i], keys)) {
      return fail(
        s,
        &t->u.tab.value[i],
        "takes no key `%.*s`",
        t->u.tab.len[i],
        t->u.tab.key[i]);
    }
  }
  return RYSPEC_OK;
}

static int required(schema* s, const toml_datum_t* t, const char* key)
{
  return get(t, key) ? RYSPEC_OK
                     : fail(s, t, "lacks the required key `%s`", key);
}

/* Every key of table t is an identifier. */
static int identifier_keys(schema* s, const toml_datum_t* t)
{
  for(int i = 0; i < t->u.tab.size; i++) {
    if(!is_identifier_bytes(t->u.tab.key[i], (size_t)t->u.tab.len[i])) {
      return fail(
        s,
        &t->u.tab.value[i],
        "key `%.*s` is not an identifier: a letter or `_`, then "
        "letters, digits or `_`",
        t->u.tab.len[i],
        t->u.tab.key[i]);
    }
  }
  return RYSPEC_OK;
}

typedef int (*value_fn)(schema* s, const toml_datum_t* d);

/* Check the value of key in table t, when there is one, with fn. */
static int at_key(
  schema* s, const toml_datum_t* t, const char* key, value_fn fn)
{
  const toml_datum_t* d = get(t, key);
  if(!d) {
    return RYSPEC_OK;
  }
  size_t before = push(s, key, (int)strlen(key), 0);
  int st = fn(s, d);
  pop(s, before);
  return st;
}

/* A table keyed by identifiers, each value checked with fn. */
static int keyed(schema* s, const toml_datum_t* t, value_fn fn)
{
  TRY(table(s, t));
  TRY(identifier_keys(s, t));
  for(int i = 0; i < t->u.tab.size; i++) {
    size_t before = push(s, t->u.tab.key[i], t->u.tab.len[i], 0);
    int st = fn(s, &t->u.tab.value[i]);
    pop(s, before);
    TRY(st);
  }
  return RYSPEC_OK;
}

/* An array of at least min items, each checked with fn, no two the same
 * string. */
static int names(schema* s, const toml_datum_t* a, int min, value_fn fn)
{
  TRY(expect(s, a, a->type == TOML_ARRAY, "an array"));
  if(a->u.arr.size < min) {
    return fail(s, a, "must hold at least %d item%s", min, min == 1 ? "" : "s");
  }
  for(int i = 0; i < a->u.arr.size; i++) {
    const toml_datum_t* e = &a->u.arr.elem[i];
    size_t before = push(s, NULL, 0, i);
    int st = fn(s, e);
    pop(s, before);
    TRY(st);
    for(int j = 0; j < i; j++) {
      const toml_datum_t* f = &a->u.arr.elem[j];
      if(
        f->u.str.len == e->u.str.len &&
        memcmp(f->u.str.ptr, e->u.str.ptr, (size_t)e->u.str.len) == 0) {
        return fail(s, e, "lists `%s` twice", e->u.s);
      }
    }
  }
  return RYSPEC_OK;
}

static int free_form(schema* s, const toml_datum_t* d)
{
  return table(s, d); /* extras: anything inside */
}

/* ---------------------------------------------------------------------------
 * Rules. */

static int prefix_rule(schema* s, const toml_datum_t* d);

/* The i-th item of the rule array a, checked with fn. */
static int item(schema* s, const toml_datum_t* a, int i, value_fn fn)
{
  size_t before = push(s, NULL, 0, i);
  int st = fn(s, &a->u.arr.elem[i]);
  pop(s, before);
  return st;
}

static int bound_end(schema* s, const toml_datum_t* d)
{
  if(d->type == TOML_STRING) {
    return identifier(s, d);
  }
  TRY(expect(s, d, is_number(d), "a number or the name of a parameter"));
  return number(d) < 0 ? fail(s, d, "must not be negative") : RYSPEC_OK;
}

static int bound(schema* s, const toml_datum_t* d)
{
  static const char* const keys[] = {"min", "max", "time_unit", NULL};
  TRY(expect(s, d, d->type == TOML_TABLE, "a bound, a table"));
  TRY(only(s, d, keys));
  if(d->u.tab.size == 0) {
    return fail(s, d, "a bound gives `min`, `max` or both");
  }
  TRY(at_key(s, d, "min", bound_end));
  TRY(at_key(s, d, "max", bound_end));
  TRY(at_key(s, d, "time_unit", time_unit));
  if(get(d, "time_unit") && !get(d, "min") && !get(d, "max")) {
    return fail(s, d, "a `time_unit` comes with `min` or `max`");
  }
  return RYSPEC_OK;
}

/* A literal { value = ... }, its value a number, a string, or either. */
static int literal(schema* s, const toml_datum_t* d, bool numbers, bool strings)
{
  static const char* const keys[] = {"value", NULL};
  const char* want = numbers && strings ? "a name or a literal, a table"
                     : numbers          ? "a name or a number literal, a table"
                                        : "a name or a string literal, a table";
  TRY(expect(s, d, d->type == TOML_TABLE, want));
  TRY(required(s, d, "value"));
  TRY(only(s, d, keys));
  const toml_datum_t* v = get(d, "value");
  size_t before = push(s, "value", 5, 0);
  int st = expect(
    s,
    v,
    (numbers && is_number(v)) || (strings && v->type == TOML_STRING),
    numbers && strings ? "a number or a string"
    : numbers          ? "a number"
                       : "a string");
  pop(s, before);
  return st;
}

static int number_operand(schema* s, const toml_datum_t* d)
{
  return d->type == TOML_STRING ? identifier(s, d) : literal(s, d, true, false);
}

static int string_operand(schema* s, const toml_datum_t* d)
{
  return d->type == TOML_STRING ? identifier(s, d) : literal(s, d, false, true);
}

static int any_operand(schema* s, const toml_datum_t* d)
{
  return d->type == TOML_STRING ? identifier(s, d) : literal(s, d, true, true);
}

static int quantified_variable(schema* s, const toml_datum_t* d)
{
  static const char* const keys[] = {"qvar", NULL};
  TRY(expect(
    s, d, d->type == TOML_TABLE, "a quantified variable, { qvar = \"...\" }"));
  TRY(required(s, d, "qvar"));
  TRY(only(s, d, keys));
  return at_key(s, d, "qvar", identifier);
}

static int qvar_name(schema* s, const toml_datum_t* d)
{
  return identifier(s, d);
}

static int qvars(schema* s, const toml_datum_t* d)
{
  return names(s, d, 1, qvar_name);
}

static int binding(schema* s, const toml_datum_t* d)
{
  static const char* const keys[] = {"qvars", NULL};
  TRY(expect(s, d, d->type == TOML_TABLE, "a binding, { qvars = [...] }"));
  TRY(required(s, d, "qvars"));
  TRY(only(s, d, keys));
  return at_key(s, d, "qvars", qvars);
}

/* What follows each family's operator, for a message about its arity. */
static const char* takes(ryspec_rule_op op)
{
  switch(op) {
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

static int operator_rule(schema* s, const toml_datum_t* a)
{
  const toml_datum_t* head = &a->u.arr.elem[0];
  ryspec_rule_op op;
  if(head->type != TOML_STRING || !ryspec_rule_op_from_name(head->u.s, &op)) {
    size_t before = push(s, NULL, 0, 0);
    int st = head->type == TOML_STRING
               ? fail(s, head, "`%s` is not an operator", head->u.s)
               : fail(
                   s,
                   head,
                   "a rule array begins with its operator, not %s",
                   type_name(head));
    pop(s, before);
    return st;
  }
  int n = a->u.arr.size - 1; /* after the operator */
  int lo, hi;
  switch(op) {
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
  if(n < lo || n > hi) {
    return fail(
      s,
      a,
      "`%s` takes %s, and has %d item%s after it",
      head->u.s,
      takes(op),
      n,
      n == 1 ? "" : "s");
  }
  switch(op) {
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
      for(int i = 1; i <= n; i++) {
        TRY(item(s, a, i, prefix_rule));
      }
      return RYSPEC_OK;
  }
}

static int prefix_rule(schema* s, const toml_datum_t* d)
{
  if(d->type == TOML_STRING) {
    return dotted_path(s, d);
  }
  TRY(expect(s, d, d->type == TOML_ARRAY, "a rule: a name or an array"));
  if(d->u.arr.size == 0) {
    return fail(s, d, "a rule array holds at least its operator");
  }
  return operator_rule(s, d);
}

/* A rule position: an expression-form string, or a prefix rule. The
 * parser translates every expression at one, so a string left beginning
 * with `(` is an expression only in a document parsed some other way. */
static int rule(schema* s, const toml_datum_t* d)
{
  if(d->type == TOML_STRING && d->u.str.len > 0 && d->u.str.ptr[0] == '(') {
    const char* p = d->u.str.ptr;
    size_t len = (size_t)d->u.str.len;
    bool blank = true;
    for(size_t i = 1; i + 1 < len; i++) {
      blank = blank && strchr(" \t\r\n", p[i]) != NULL;
    }
    if(len < 3 || p[len - 1] != ')' || blank) {
      return fail(s, d, "an expression is a non-empty `(...)`");
    }
    return RYSPEC_OK;
  }
  return prefix_rule(s, d);
}

static int rule_table(schema* s, const toml_datum_t* d)
{
  return keyed(s, d, rule);
}

/* ---------------------------------------------------------------------------
 * Properties and namespaces. */

/* Whitespace to a regex's \s: ECMA-262's, which JSON Schema names, and
 * Python's, which jsonschema runs, U+001C to U+001F and U+0085 among
 * them; so a tag either refuses is refused here. */
static bool is_space(unsigned long c)
{
  return (c >= 0x09 && c <= 0x0d) || (c >= 0x1c && c <= 0x20) || c == 0x85 ||
         c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) ||
         c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f ||
         c == 0x3000 || c == 0xfeff;
}

/* Whether the UTF-8 of len bytes at p, which the parser has checked,
 * holds whitespace. */
static bool has_whitespace(const char* p, size_t len)
{
  const unsigned char* u = (const unsigned char*)p;
  for(size_t i = 0; i < len;) {
    unsigned long c = u[i];
    size_t n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    if(n > 1) {
      c &= 0x3fu >> (n - 1);
      for(size_t k = 1; k < n && i + k < len; k++) {
        c = (c << 6) | (u[i + k] & 0x3fu);
      }
    }
    if(is_space(c)) {
      return true;
    }
    i += n;
  }
  return false;
}

static int tag(schema* s, const toml_datum_t* d)
{
  TRY(string(s, d));
  if(d->u.str.len == 0 || has_whitespace(d->u.str.ptr, (size_t)d->u.str.len)) {
    return fail(s, d, "a tag is one word, without whitespace");
  }
  return RYSPEC_OK;
}

static int tags(schema* s, const toml_datum_t* d)
{
  return names(s, d, 0, tag);
}

static int property(schema* s, const toml_datum_t* d)
{
  static const char* const keys[] = {
    "title",
    "description",
    "message",
    "time_unit",
    "tags",
    "given",
    "check",
    "where",
    "extras",
    NULL};
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

static int property_table(schema* s, const toml_datum_t* d)
{
  return keyed(s, d, property);
}

static int namespace(schema* s, const toml_datum_t* d)
{
  static const char* const leaf_keys[] = {
    "rules", "properties", "extras", NULL};
  TRY(table(s, d));
  TRY(identifier_keys(s, d));
  if(get(d, "rules") || get(d, "properties") || get(d, "extras")) {
    /* A leaf: logic, and no nested namespace. */
    TRY(only(s, d, leaf_keys));
    TRY(at_key(s, d, "rules", rule_table));
    TRY(at_key(s, d, "properties", property_table));
    return at_key(s, d, "extras", free_form);
  }
  return keyed(s, d, namespace);
}

static int namespaces(schema* s, const toml_datum_t* d)
{
  static const char* const own[] = {"rules", "properties", "extras", NULL};
  TRY(table(s, d));
  TRY(identifier_keys(s, d));
  for(int i = 0; i < d->u.tab.size; i++) {
    if(listed(d->u.tab.key[i], d->u.tab.len[i], own)) {
      return fail(
        s,
        &d->u.tab.value[i],
        "takes no key `%.*s`: `namespace` is no namespace, and the "
        "anonymous one's is at the root",
        d->u.tab.len[i],
        d->u.tab.key[i]);
    }
  }
  return keyed(s, d, namespace);
}

/* ---------------------------------------------------------------------------
 * Variables and monitors. */

static int boolean(schema* s, const toml_datum_t* d)
{
  return expect(s, d, d->type == TOML_BOOLEAN, "a boolean");
}

static int a_number(schema* s, const toml_datum_t* d)
{
  return expect(s, d, is_number(d), "a number");
}

static int text_format(schema* s, const toml_datum_t* d)
{
  static const char* const formats[] = {"json", NULL};
  return one_of(s, d, formats, "json");
}

static int binary_format(schema* s, const toml_datum_t* d)
{
  static const char* const formats[] = {"flatbuffers", NULL};
  return one_of(s, d, formats, "flatbuffers");
}

static int variable(schema* s, const toml_datum_t* d)
{
  static const char* const types[] = {"bool", "number", "text", "binary", NULL};
  static const char* const bool_keys[] = {
    "type", "source", "title", "description", "extras", "initial_value", NULL};
  static const char* const number_keys[] = {
    "type",
    "source",
    "title",
    "description",
    "extras",
    "initial_value",
    "min",
    "max",
    NULL};
  static const char* const format_keys[] = {
    "type", "source", "title", "description", "extras", "format", NULL};
  TRY(table(s, d));
  const toml_datum_t* type = get(d, "type");
  if(type) {
    size_t before = push(s, "type", 4, 0);
    int st = one_of(s, type, types, "bool, number, text, binary");
    pop(s, before);
    TRY(st);
  }
  const char* t = type ? type->u.s : "bool";
  TRY(at_key(s, d, "source", dotted_path));
  TRY(at_key(s, d, "title", string));
  TRY(at_key(s, d, "description", string));
  TRY(at_key(s, d, "extras", free_form));
  if(strcmp(t, "bool") == 0) {
    TRY(only(s, d, bool_keys));
    return at_key(s, d, "initial_value", boolean);
  }
  if(strcmp(t, "number") == 0) {
    TRY(only(s, d, number_keys));
    TRY(at_key(s, d, "initial_value", a_number));
    TRY(at_key(s, d, "min", a_number));
    return at_key(s, d, "max", a_number);
  }
  TRY(only(s, d, format_keys));
  return at_key(
    s, d, "format", strcmp(t, "text") == 0 ? text_format : binary_format);
}

static int variables(schema* s, const toml_datum_t* d)
{
  return keyed(s, d, variable);
}

static int at_least_one(schema* s, const toml_datum_t* d)
{
  TRY(expect(s, d, is_integer(d), "an integer"));
  return number(d) < 1 ? fail(s, d, "must be at least 1") : RYSPEC_OK;
}

static int positive(schema* s, const toml_datum_t* d)
{
  TRY(a_number(s, d));
  return number(d) <= 0 ? fail(s, d, "must be greater than 0") : RYSPEC_OK;
}

static int runtime(schema* s, const toml_datum_t* d)
{
  static const char* const keys[] = {
    "allocation_size", "buffer_size", "base_period", "time_unit", NULL};
  TRY(table(s, d));
  TRY(only(s, d, keys));
  TRY(at_key(s, d, "allocation_size", at_least_one));
  TRY(at_key(s, d, "buffer_size", at_least_one));
  TRY(at_key(s, d, "base_period", positive));
  TRY(at_key(s, d, "time_unit", time_unit));
  if(get(d, "base_period") && !get(d, "time_unit")) {
    return fail(
      s,
      get(d, "base_period"),
      "a `base_period` is in the monitor's `time_unit`, which is "
      "missing");
  }
  return RYSPEC_OK;
}

static int identifiers(schema* s, const toml_datum_t* d)
{
  return names(s, d, 0, identifier);
}

static int outputs(schema* s, const toml_datum_t* d)
{
  return names(s, d, 1, dotted_path);
}

static int monitor(schema* s, const toml_datum_t* d)
{
  static const char* const keys[] = {
    "runtime", "inputs", "outputs", "parameters", NULL};
  TRY(table(s, d));
  TRY(required(s, d, "outputs"));
  TRY(only(s, d, keys));
  TRY(at_key(s, d, "runtime", runtime));
  TRY(at_key(s, d, "inputs", identifiers));
  TRY(at_key(s, d, "outputs", outputs));
  return at_key(s, d, "parameters", identifiers);
}

static int monitors(schema* s, const toml_datum_t* d)
{
  return keyed(s, d, monitor);
}

/* ---------------------------------------------------------------------------
 * The document. */

static int version(schema* s, const toml_datum_t* d)
{
  TRY(string(s, d));
  return strcmp(d->u.s, "0") == 0
           ? RYSPEC_OK
           : fail(s, d, "must be \"0\", not \"%s\"", d->u.s);
}

static int meta(schema* s, const toml_datum_t* d)
{
  static const char* const keys[] = {"title", "description", NULL};
  TRY(table(s, d));
  TRY(only(s, d, keys));
  TRY(at_key(s, d, "title", string));
  return at_key(s, d, "description", string);
}

int ryspec_lint_check_schema(const ryspec_toml_doc* doc, ryspec_diag* diag)
{
  static const char* const keys[] = {
    "version",
    "meta",
    "variables",
    "monitors",
    "rules",
    "properties",
    "namespace",
    "extras",
    NULL};
  schema sc = {.diag = diag};
  schema* s = &sc;
  const toml_datum_t* root = ryspec_toml_doc_root(doc);
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

/* ===========================================================================
 * The checks of monitors: Rules 1, 5, 6 and 18. */

/* The monitor m's list of key, or NULL when it lacks one or it is no
 * array. */
static const toml_datum_t* list(const toml_datum_t* m, const char* key)
{
  const toml_datum_t* v = get(m, key);
  return v && v->type == TOML_ARRAY ? v : NULL;
}

/* Whether the array a, which may be NULL, holds the string of len bytes at
 * name. */
static bool holds(const toml_datum_t* a, const char* name, size_t len)
{
  for(int i = 0; a && i < a->u.arr.size; i++) {
    const toml_datum_t* e = &a->u.arr.elem[i];
    if(
      e->type == TOML_STRING && (size_t)e->u.str.len == len &&
      memcmp(e->u.str.ptr, name, len) == 0) {
      return true;
    }
  }
  return false;
}

/* Whether the monitor m lists the len bytes at name in any of its lists. */
static bool lists(const toml_datum_t* m, const char* name, size_t len)
{
  return holds(list(m, "inputs"), name, len) ||
         holds(list(m, "parameters"), name, len) ||
         holds(list(m, "outputs"), name, len);
}

/* Rule 1: a name appears in at most one of a monitor's lists. Reported at
 * its entry in the later list. */
int ryspec_lint_check_monitor_list_overlap(
  const ryspec_toml_doc* doc, ryspec_diag* diag)
{
  static const char* names[] = {"inputs", "parameters", "outputs"};
  decl m;
  for(int i = 0; next_decl(monitors_of(doc), &i, &m); i++) {
    const toml_datum_t* lists[] = {
      list(m.value, names[0]),
      list(m.value, names[1]),
      list(m.value, names[2])};
    for(int b = 1; b < 3; b++) {
      for(int k = 0; lists[b] && k < lists[b]->u.arr.size; k++) {
        const toml_datum_t* e = &lists[b]->u.arr.elem[k];
        if(e->type != TOML_STRING) {
          continue;
        }
        for(int a = 0; a < b; a++) {
          if(holds(lists[a], e->u.str.ptr, (size_t)e->u.str.len)) {
            return ryspec_lint_fail(
              diag,
              e,
              "`%s` is in both the %s and the %s of monitor "
              "`%.*s`",
              e->u.s,
              names[a],
              names[b],
              m.len,
              m.name);
          }
        }
      }
    }
  }
  return RYSPEC_OK;
}

/* Rule 5: an initial_value sits only on a variable a monitor lists. */
int ryspec_lint_check_initial_value_listed(
  const ryspec_toml_doc* doc, ryspec_diag* diag)
{
  decl v;
  for(int i = 0; next_decl(variables_of(doc), &i, &v); i++) {
    const toml_datum_t* initial = get(v.value, "initial_value");
    if(!initial) {
      continue;
    }
    bool listed = false;
    decl m;
    for(int j = 0; !listed && next_decl(monitors_of(doc), &j, &m); j++) {
      listed = lists(m.value, v.name, (size_t)v.len);
    }
    if(!listed) {
      return ryspec_lint_fail(
        diag,
        initial,
        "`%.*s` has an initial_value, but no monitor lists it",
        v.len,
        v.name);
    }
  }
  return RYSPEC_OK;
}

/* Each declared variable a parameters entry names, with the entry: fn is
 * called on each until it returns anything but RYSPEC_OK. */
typedef int (*parameter_fn)(
  const decl* m, const toml_datum_t* entry, const decl* v, ryspec_diag* diag);

static int each_parameter(
  const ryspec_toml_doc* doc, parameter_fn fn, ryspec_diag* diag)
{
  decl m;
  for(int i = 0; next_decl(monitors_of(doc), &i, &m); i++) {
    const toml_datum_t* parameters = list(m.value, "parameters");
    for(int k = 0; parameters && k < parameters->u.arr.size; k++) {
      const toml_datum_t* e = &parameters->u.arr.elem[k];
      if(e->type != TOML_STRING) {
        continue;
      }
      decl v = {
        e->u.str.len,
        e->u.str.ptr,
        ryspec_toml_doc_variable(doc, e->u.str.ptr, (size_t)e->u.str.len)};
      if(!v.value) {
        continue; /* Rule 3's */
      }
      int s = fn(&m, e, &v, diag);
      if(s != RYSPEC_OK) {
        return s;
      }
    }
  }
  return RYSPEC_OK;
}

static int has_initial(
  const decl* m, const toml_datum_t* entry, const decl* v, ryspec_diag* diag)
{
  if(get(v->value, "initial_value")) {
    return RYSPEC_OK;
  }
  return ryspec_lint_fail(
    diag,
    entry,
    "`%.*s` is a parameter of monitor `%.*s` and has no "
    "initial_value",
    v->len,
    v->name,
    m->len,
    m->name);
}

/* Rule 6: a listed parameter carries an initial_value. Reported at its
 * entry. */
int ryspec_lint_check_parameter_initial(
  const ryspec_toml_doc* doc, ryspec_diag* diag)
{
  return each_parameter(doc, has_initial, diag);
}

static int has_no_source(
  const decl* m, const toml_datum_t* entry, const decl* v, ryspec_diag* diag)
{
  (void)entry;
  const toml_datum_t* source = get(v->value, "source");
  if(!source) {
    return RYSPEC_OK;
  }
  return ryspec_lint_fail(
    diag,
    source,
    "`%.*s` is a parameter of monitor `%.*s` and may not "
    "declare source",
    v->len,
    v->name,
    m->len,
    m->name);
}

/* Rule 18: a parameter carries no `source`. Reported at the source. */
int ryspec_lint_check_parameter_no_source(
  const ryspec_toml_doc* doc, ryspec_diag* diag)
{
  return each_parameter(doc, has_no_source, diag);
}

/* Whether a bare reference of some rule, of len bytes at name, resolves to
 * nothing declared, so that deduction makes it an input. */
typedef struct deduced {
  const ryspec_toml_doc* doc;
  const char* name;
  size_t len;
} deduced;

static int is_deduced(const ryspec_lint_node* n, void* ctx, ryspec_diag* diag)
{
  (void)diag;
  const deduced* d = ctx;
  if(
    n->kind != RYSPEC_LINT_REFERENCE || (size_t)n->datum->u.str.len != d->len ||
    memcmp(n->datum->u.str.ptr, d->name, d->len) != 0) {
    return RYSPEC_OK;
  }
  ryspec_toml_doc_target t =
    ryspec_toml_doc_resolve(d->doc, n->position, d->name, d->len);
  /* Any status but RYSPEC_OK stops the walk: found. */
  return t.resolution == RYSPEC_RESOLVES_DEDUCED ? RYSPEC_ERROR_SEMANTIC
                                                 : RYSPEC_OK;
}

static bool used_as_input(
  const ryspec_toml_doc* doc, const char* name, size_t len)
{
  deduced d = {doc, name, len};
  return ryspec_lint_walk_all(doc, is_deduced, &d, NULL) != RYSPEC_OK;
}

static int output_resolves(
  const ryspec_toml_doc* doc,
  const decl* m,
  const toml_datum_t* e,
  ryspec_diag* diag)
{
  const char* name = e->u.str.ptr;
  size_t len = (size_t)e->u.str.len;
  ryspec_toml_doc_target t = ryspec_toml_doc_resolve(doc, NULL, name, len);
  if(t.resolution == RYSPEC_RESOLVES_AMBIGUOUS) {
    return RYSPEC_OK; /* Rule 11's */
  }
  switch(ryspec_toml_doc_target_kind(t)) {
    case RYSPEC_ENTITY_PROPERTY:
    case RYSPEC_ENTITY_NAMESPACE: /* Rule 22's */
      return RYSPEC_OK;
    case RYSPEC_ENTITY_RULE:
      return ryspec_lint_fail(
        diag,
        e,
        "output `%s` of monitor `%.*s` names a rule, and a "
        "monitor publishes properties and computed variables",
        name,
        m->len,
        m->name);
    case RYSPEC_ENTITY_VARIABLE:
      if(get(t.entity.value, "source")) {
        return RYSPEC_OK;
      }
      return ryspec_lint_fail(
        diag,
        e,
        "output `%s` of monitor `%.*s` names a variable with no "
        "source, which nothing computes",
        name,
        m->len,
        m->name);
    default:
      return ryspec_lint_fail(
        diag,
        e,
        "output `%s` of monitor `%.*s` names no property or "
        "computed variable",
        name,
        m->len,
        m->name);
  }
}

/* Rule 3: every monitor entry resolves. An input names a declared variable
 * or a name deduction makes an input, a parameter a declared variable, and
 * an output a property or a computed variable. */
int ryspec_lint_check_monitor_entry_resolves(
  const ryspec_toml_doc* doc, ryspec_diag* diag)
{
  decl m;
  for(int i = 0; next_decl(monitors_of(doc), &i, &m); i++) {
    const toml_datum_t* lists[] = {
      list(m.value, "inputs"),
      list(m.value, "parameters"),
      list(m.value, "outputs")};
    static const char* what[] = {"input", "parameter"};
    for(int l = 0; l < 3; l++) {
      for(int k = 0; lists[l] && k < lists[l]->u.arr.size; k++) {
        const toml_datum_t* e = &lists[l]->u.arr.elem[k];
        if(e->type != TOML_STRING) {
          continue;
        }
        const char* name = e->u.str.ptr;
        size_t len = (size_t)e->u.str.len;
        if(l == 2) {
          int s = output_resolves(doc, &m, e, diag);
          if(s != RYSPEC_OK) {
            return s;
          }
          continue;
        }
        if(
          ryspec_toml_doc_variable(doc, name, len) ||
          (l == 0 && used_as_input(doc, name, len))) {
          continue;
        }
        return ryspec_lint_fail(
          diag,
          e,
          "%s `%s` of monitor `%.*s` names no "
          "declared variable",
          what[l],
          name,
          m.len,
          m.name);
      }
    }
  }
  return RYSPEC_OK;
}

/* ===========================================================================
 * The checks of numbers: Rules 7 and 23. */

/* The number d holds in *out, returning whether it is one. */
static bool to_number(const toml_datum_t* d, double* out)
{
  if(!d) {
    return false;
  }
  if(d->type == TOML_INT64) {
    *out = (double)d->u.int64;
    return true;
  }
  if(d->type == TOML_FP64) {
    *out = d->u.fp64;
    return true;
  }
  return false;
}

static bool is_nan(const toml_datum_t* d)
{
  return d && d->type == TOML_FP64 && isnan(d->u.fp64);
}

/* Fail at at when table t's min is greater than its max, both numbers. */
static int ordered(
  const toml_datum_t* t,
  const toml_datum_t* at,
  const char* what,
  ryspec_diag* diag)
{
  const toml_datum_t *min = get(t, "min"), *max = get(t, "max");
  double lo, hi;
  if(!to_number(min, &lo) || !to_number(max, &hi) || !(lo > hi)) {
    return RYSPEC_OK;
  }
  char lo_text[32], hi_text[32];
  ryspec_lint_number_text(min, lo_text, sizeof lo_text);
  ryspec_lint_number_text(max, hi_text, sizeof hi_text);
  return ryspec_lint_fail(
    diag, at, "%s min (%s) is greater than max (%s)", what, lo_text, hi_text);
}

static int bound_ordered(
  const ryspec_lint_node* n, void* ctx, ryspec_diag* diag)
{
  (void)ctx;
  if(n->kind != RYSPEC_LINT_BOUND) {
    return RYSPEC_OK;
  }
  return ordered(n->datum, n->at, "the bound's", diag);
}

/* Rule 7: min is not greater than max, on a number variable or on a metric
 * bound. */
int ryspec_lint_check_min_le_max(const ryspec_toml_doc* doc, ryspec_diag* diag)
{
  decl v;
  for(int i = 0; next_decl(variables_of(doc), &i, &v); i++) {
    if(ryspec_toml_doc_variable_type(v.value) != RYSPEC_TYPE_NUMBER) {
      continue;
    }
    char what[128];
    snprintf(what, sizeof what, "`%.*s`'s", v.len, v.name);
    const toml_datum_t* min = get(v.value, "min");
    int s = ordered(v.value, min ? min : v.value, what, diag);
    if(s != RYSPEC_OK) {
      return s;
    }
  }
  return ryspec_lint_walk_all(doc, bound_ordered, NULL, diag);
}

static int no_nan_in_rule(
  const ryspec_lint_node* n, void* ctx, ryspec_diag* diag)
{
  (void)ctx;
  if(n->kind == RYSPEC_LINT_LITERAL && is_nan(get(n->datum, "value"))) {
    return ryspec_lint_fail(diag, n->at, "a literal is nan, and no number is");
  }
  if(n->kind == RYSPEC_LINT_BOUND) {
    static const char* ends[] = {"min", "max"};
    for(int i = 0; i < 2; i++) {
      if(is_nan(get(n->datum, ends[i]))) {
        return ryspec_lint_fail(
          diag, n->at, "the bound's %s is nan, and no number is", ends[i]);
      }
    }
  }
  return RYSPEC_OK;
}

/* Rule 23: no number is nan: a variable's initial_value, min or max, a
 * monitor's base_period, a literal or a bound's end. */
int ryspec_lint_check_no_nan(const ryspec_toml_doc* doc, ryspec_diag* diag)
{
  static const char* keys[] = {"initial_value", "min", "max"};
  decl v;
  for(int i = 0; next_decl(variables_of(doc), &i, &v); i++) {
    for(int k = 0; k < 3; k++) {
      const toml_datum_t* d = get(v.value, keys[k]);
      if(is_nan(d)) {
        return ryspec_lint_fail(
          diag,
          d,
          "`%.*s`'s %s is nan, and no number is",
          v.len,
          v.name,
          keys[k]);
      }
    }
  }
  decl m;
  for(int i = 0; next_decl(monitors_of(doc), &i, &m); i++) {
    const toml_datum_t* period = get(get(m.value, "runtime"), "base_period");
    if(is_nan(period)) {
      return ryspec_lint_fail(
        diag,
        period,
        "monitor `%.*s`'s base_period is nan, and no number is",
        m.len,
        m.name);
    }
  }
  return ryspec_lint_walk_all(doc, no_nan_in_rule, NULL, diag);
}

/* Rule 8: a multi-segment source path has a declared head that carries a
 * format. */
int ryspec_lint_check_source_head(const ryspec_toml_doc* doc, ryspec_diag* diag)
{
  decl v;
  for(int i = 0; next_decl(variables_of(doc), &i, &v); i++) {
    const toml_datum_t* source = get(v.value, "source");
    if(!source || source->type != TOML_STRING) {
      continue;
    }
    const char* path = source->u.s;
    const char* dot = strchr(path, '.');
    if(!dot) {
      continue;
    }
    int head_len = (int)(dot - path);
    const toml_datum_t* head =
      ryspec_toml_doc_variable(doc, path, (size_t)head_len);
    if(!head) {
      return ryspec_lint_fail(
        diag,
        source,
        "`%.*s`'s source `%s` reads from `%.*s`, which no "
        "variable declares",
        v.len,
        v.name,
        path,
        head_len,
        path);
    }
    if(!get(head, "format")) {
      return ryspec_lint_fail(
        diag,
        source,
        "`%.*s`'s source `%s` reads a field of `%.*s`, which "
        "declares no format",
        v.len,
        v.name,
        path,
        head_len,
        path);
    }
  }
  return RYSPEC_OK;
}

/* ===========================================================================
 * The checks of names and their types: Rules 9, 11, 19, 20 and 22. */

static ryspec_toml_doc_target resolve(
  const ryspec_toml_doc* doc,
  const ryspec_lint_node* n,
  const toml_datum_t* name)
{
  return ryspec_toml_doc_resolve(
    doc, n->position, name->u.str.ptr, (size_t)name->u.str.len);
}

/* ---------------------------------------------------------------------------
 * Comparisons: the family a comparison is in, by its operator, and for `eq`
 * and `ne`, by its operands' types. */

typedef enum family {
  FAMILY_UNKNOWN,
  FAMILY_NUMBER,
  FAMILY_TEXT,
} family;

static family family_of(const ryspec_toml_doc* doc, const ryspec_lint_node* n)
{
  switch(n->op) {
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
  for(int i = 1; i < n->datum->u.arr.size; i++) {
    const toml_datum_t* e = &n->datum->u.arr.elem[i];
    ryspec_value_type type;
    if(e->type == TOML_TABLE) {
      const toml_datum_t* value = get(e, "value");
      if(!value) {
        continue;
      }
      type = value->type == TOML_STRING ? RYSPEC_TYPE_TEXT : RYSPEC_TYPE_NUMBER;
    }
    else if(
      e->type != TOML_STRING ||
      !ryspec_toml_doc_target_type(resolve(doc, n, e), &type)) {
      continue;
    }
    if(type == RYSPEC_TYPE_NUMBER) {
      return FAMILY_NUMBER;
    }
    if(type == RYSPEC_TYPE_TEXT) {
      f = FAMILY_TEXT;
    }
  }
  return f;
}

/* Each operand of the comparison n naming something of a type, with it:
 * fn is called on each until it returns anything but RYSPEC_OK. */
typedef int (*operand_fn)(
  const ryspec_lint_node* n,
  family f,
  const toml_datum_t* e,
  ryspec_value_type type,
  ryspec_diag* diag);

static int each_operand(
  const ryspec_toml_doc* doc,
  const ryspec_lint_node* n,
  operand_fn fn,
  ryspec_diag* diag)
{
  family f = family_of(doc, n);
  for(int i = 1; i < n->datum->u.arr.size; i++) {
    const toml_datum_t* e = &n->datum->u.arr.elem[i];
    ryspec_value_type type;
    if(
      e->type != TOML_STRING ||
      !ryspec_toml_doc_target_type(resolve(doc, n, e), &type)) {
      continue;
    }
    int s = fn(n, f, e, type, diag);
    if(s != RYSPEC_OK) {
      return s;
    }
  }
  return RYSPEC_OK;
}

static bool is_comparison(const ryspec_lint_node* n)
{
  return n->kind == RYSPEC_LINT_OPERATOR && n->known &&
         ryspec_lint_op_is_comparison(n->op);
}

/* ---------------------------------------------------------------------------
 * Rule 9: a text or binary variable is never read as a truth value, nor as a
 * number, which is as an operand of a number comparison. */

static int not_read_as_number(
  const ryspec_lint_node* n,
  family f,
  const toml_datum_t* e,
  ryspec_value_type type,
  ryspec_diag* diag)
{
  if(
    f != FAMILY_NUMBER ||
    (type != RYSPEC_TYPE_TEXT && type != RYSPEC_TYPE_BINARY)) {
    return RYSPEC_OK;
  }
  return ryspec_lint_fail(
    diag,
    placed(e, n->at),
    "`%s` is a %s variable, and `%s` reads it as a number",
    e->u.s,
    ryspec_toml_doc_type_name(type),
    ryspec_rule_op_name(n->op));
}

static int not_text_as_truth(
  const ryspec_lint_node* n, void* ctx, ryspec_diag* diag)
{
  const ryspec_toml_doc* doc = ctx;
  if(is_comparison(n)) {
    return each_operand(doc, n, not_read_as_number, diag);
  }
  if(n->kind != RYSPEC_LINT_REFERENCE || !ryspec_lint_node_is_truth(n)) {
    return RYSPEC_OK;
  }
  ryspec_toml_doc_target t = resolve(doc, n, n->datum);
  ryspec_value_type type;
  if(
    ryspec_toml_doc_target_kind(t) != RYSPEC_ENTITY_VARIABLE ||
    !ryspec_toml_doc_target_type(t, &type) ||
    (type != RYSPEC_TYPE_TEXT && type != RYSPEC_TYPE_BINARY)) {
    return RYSPEC_OK;
  }
  return ryspec_lint_fail(
    diag,
    n->at,
    "`%s` is a %s variable, and is read as a truth value",
    n->datum->u.s,
    ryspec_toml_doc_type_name(type));
}

int ryspec_lint_check_no_text_as_truth(
  const ryspec_toml_doc* doc, ryspec_diag* diag)
{
  return ryspec_lint_walk_all(doc, not_text_as_truth, (void*)doc, diag);
}

/* ---------------------------------------------------------------------------
 * Rule 11: no two sources of value share a bare name where both are
 * visible: within one namespace its rules, its properties and the rules
 * private to each property; any of those and a variable; and at the root,
 * a top-level namespace and any of the others. */

typedef struct source {
  const char* what;
  const char* name;
  int len;
  const toml_datum_t* ns;       /* its namespace's table, NULL for a variable */
  const toml_datum_t* property; /* the property it is private to, or NULL */
  const toml_datum_t* at;
} source;

/* Whether a and b are both visible where either is. */
static bool collide(const source* a, const source* b)
{
  if(a->len != b->len || memcmp(a->name, b->name, (size_t)a->len) != 0) {
    return false;
  }
  if(!a->ns || !b->ns) {
    return true; /* a variable is visible everywhere */
  }
  if(a->ns != b->ns) {
    return false;
  }
  return !a->property || !b->property || a->property == b->property;
}

/* The later of the two in the document, where a diagnostic belongs. */
static const toml_datum_t* later(const toml_datum_t* a, const toml_datum_t* b)
{
  if(!a->lineno) {
    return b;
  }
  if(!b->lineno) {
    return a;
  }
  if(a->lineno != b->lineno) {
    return a->lineno > b->lineno ? a : b;
  }
  return a->colno >= b->colno ? a : b;
}

/* The sources of a document, in the order the check meets them, which is
 * not the document's: the variables, the rules and private rules, the
 * properties, then the top-level namespaces, each in the document's
 * order. A pass gathers one group, counting them alone where sources is
 * NULL. */
typedef struct sources {
  const ryspec_toml_doc* doc;
  int group;
  source* sources;
  size_t n;
} sources;

static int group_of(ryspec_entity_kind kind)
{
  switch(kind) {
    case RYSPEC_ENTITY_VARIABLE:
      return 0;
    case RYSPEC_ENTITY_RULE:
    case RYSPEC_ENTITY_PRIVATE_RULE:
      return 1;
    case RYSPEC_ENTITY_PROPERTY:
      return 2;
    case RYSPEC_ENTITY_NAMESPACE:
      return 3;
    default:
      return -1;
  }
}

static const char* source_what(ryspec_entity_kind kind)
{
  switch(kind) {
    case RYSPEC_ENTITY_VARIABLE:
      return "a variable";
    case RYSPEC_ENTITY_RULE:
      return "a rule";
    case RYSPEC_ENTITY_PRIVATE_RULE:
      return "a private rule";
    case RYSPEC_ENTITY_PROPERTY:
      return "a property";
    default:
      return "a namespace";
  }
}

static int gather(const ryspec_toml_doc_entity* e, void* ctx, ryspec_diag* diag)
{
  (void)diag;
  sources* g = ctx;
  int group = group_of(e->kind);
  /* A namespace but a top-level one is no source of value. */
  if(
    group < 0 || (g->group >= 0 && group != g->group) ||
    (e->kind == RYSPEC_ENTITY_NAMESPACE &&
     e->ns != ryspec_toml_doc_root(g->doc))) {
    return RYSPEC_OK;
  }
  if(g->sources) {
    g->sources[g->n] = (source){
      source_what(e->kind), e->name, e->len, e->ns, e->property, e->value};
  }
  g->n++;
  return RYSPEC_OK;
}

int ryspec_lint_check_one_source_of_value(
  const ryspec_toml_doc* doc, ryspec_diag* diag)
{
  /* Each source read from the tree once, and not per pair. */
  toml_option_t opt = ryspec_toml_options();
  sources g = {doc, -1, NULL, 0};
  ryspec_toml_doc_each_entity(doc, gather, &g, NULL);
  g.sources = opt.mem_realloc(NULL, (g.n ? g.n : 1) * sizeof *g.sources);
  if(!g.sources) {
    ryspec_diagnose(diag, RYSPEC_ERROR_MEMORY, 0, 0, "out of memory");
    return RYSPEC_ERROR_MEMORY;
  }
  g.n = 0;
  for(g.group = 0; g.group < 4; g.group++) {
    ryspec_toml_doc_each_entity(doc, gather, &g, NULL);
  }
  int s = RYSPEC_OK;
  for(size_t i = 0; s == RYSPEC_OK && i < g.n; i++) {
    const source* a = &g.sources[i];
    for(size_t j = 0; j < i; j++) {
      const source* b = &g.sources[j];
      if((!a->ns && !b->ns) || !collide(a, b)) {
        continue;
      }
      bool a_later = later(a->at, b->at) == a->at;
      s = ryspec_lint_fail(
        diag,
        a_later ? a->at : b->at,
        "`%.*s` has more than one source of value: %s and %s",
        a->len,
        a->name,
        a_later ? b->what : a->what,
        a_later ? a->what : b->what);
      break;
    }
  }
  opt.mem_free(g.sources);
  return s;
}

/* ---------------------------------------------------------------------------
 * Rule 19: a bound end that is a name resolves to a number parameter. */

static int bound_names_number(
  const ryspec_lint_node* n, void* ctx, ryspec_diag* diag)
{
  const ryspec_toml_doc* doc = ctx;
  if(n->kind != RYSPEC_LINT_BOUND) {
    return RYSPEC_OK;
  }
  static const char* ends[] = {"min", "max"};
  for(int i = 0; i < 2; i++) {
    const toml_datum_t* end = get(n->datum, ends[i]);
    if(!end || end->type != TOML_STRING) {
      continue;
    }
    ryspec_toml_doc_target t = resolve(doc, n, end);
    if(t.resolution == RYSPEC_RESOLVES_AMBIGUOUS) {
      continue; /* Rule 11's */
    }
    ryspec_value_type type;
    if(
      ryspec_toml_doc_target_kind(t) == RYSPEC_ENTITY_VARIABLE &&
      ryspec_toml_doc_target_type(t, &type) && type == RYSPEC_TYPE_NUMBER) {
      continue;
    }
    if(ryspec_toml_doc_target_kind(t) == RYSPEC_ENTITY_VARIABLE) {
      return ryspec_lint_fail(
        diag,
        placed(end, n->at),
        "the bound's %s `%s` is a %s variable, with no "
        "comparable value: a bound names a number parameter",
        ends[i],
        end->u.s,
        ryspec_toml_doc_type_name(type));
    }
    return ryspec_lint_fail(
      diag,
      placed(end, n->at),
      "the bound's %s `%s` names no number parameter, so has "
      "no comparable value",
      ends[i],
      end->u.s);
  }
  return RYSPEC_OK;
}

int ryspec_lint_check_bound_name_number(
  const ryspec_toml_doc* doc, ryspec_diag* diag)
{
  return ryspec_lint_walk_all(doc, bound_names_number, (void*)doc, diag);
}

/* ---------------------------------------------------------------------------
 * Rule 20: a comparison's operands have its family's type. A text or binary
 * operand of a number comparison is Rule 9's. */

static int fits_family(
  const ryspec_lint_node* n,
  family f,
  const toml_datum_t* e,
  ryspec_value_type type,
  ryspec_diag* diag)
{
  const char* op = ryspec_rule_op_name(n->op);
  switch(f) {
    case FAMILY_NUMBER:
      if(
        type == RYSPEC_TYPE_NUMBER || type == RYSPEC_TYPE_TEXT ||
        type == RYSPEC_TYPE_BINARY) {
        return RYSPEC_OK;
      }
      return ryspec_lint_fail(
        diag,
        placed(e, n->at),
        "`%s` is a %s, and `%s` here compares numbers",
        e->u.s,
        ryspec_toml_doc_type_name(type),
        op);
    case FAMILY_TEXT:
      if(type == RYSPEC_TYPE_TEXT) {
        return RYSPEC_OK;
      }
      return ryspec_lint_fail(
        diag,
        placed(e, n->at),
        "`%s` is a %s, and `%s` here compares text",
        e->u.s,
        ryspec_toml_doc_type_name(type),
        op);
    default:
      return ryspec_lint_fail(
        diag,
        placed(e, n->at),
        "`%s` is a %s, and `%s` compares numbers or text",
        e->u.s,
        ryspec_toml_doc_type_name(type),
        op);
  }
}

static int comparison_fits(
  const ryspec_lint_node* n, void* ctx, ryspec_diag* diag)
{
  if(!is_comparison(n)) {
    return RYSPEC_OK;
  }
  return each_operand(ctx, n, fits_family, diag);
}

int ryspec_lint_check_comparison_types(
  const ryspec_toml_doc* doc, ryspec_diag* diag)
{
  return ryspec_lint_walk_all(doc, comparison_fits, (void*)doc, diag);
}

/* ---------------------------------------------------------------------------
 * Rule 22: a name resolves to a variable, a rule or a property, and never
 * to a namespace; a dotted path, which deduction never makes an input,
 * resolves to one of them too. */

static int names_no_namespace(
  const ryspec_lint_node* n, void* ctx, ryspec_diag* diag)
{
  const ryspec_toml_doc* doc = ctx;
  if(n->kind != RYSPEC_LINT_REFERENCE) {
    return RYSPEC_OK;
  }
  ryspec_toml_doc_target t = resolve(doc, n, n->datum);
  if(ryspec_toml_doc_target_kind(t) == RYSPEC_ENTITY_NAMESPACE) {
    return ryspec_lint_fail(
      diag,
      n->at,
      "`%s` names a namespace, and a name resolves to a "
      "variable, a rule or a property",
      n->datum->u.s);
  }
  if(t.resolution == RYSPEC_RESOLVES_NOTHING) {
    return ryspec_lint_fail(
      diag, n->at, "`%s` resolves to no rule and no property", n->datum->u.s);
  }
  return RYSPEC_OK;
}

int ryspec_lint_check_no_namespace_reference(
  const ryspec_toml_doc* doc, ryspec_diag* diag)
{
  decl m;
  for(int i = 0; next_decl(monitors_of(doc), &i, &m); i++) {
    const toml_datum_t* outputs = list(m.value, "outputs");
    for(int k = 0; outputs && k < outputs->u.arr.size; k++) {
      const toml_datum_t* e = &outputs->u.arr.elem[k];
      if(
        e->type == TOML_STRING &&
        ryspec_toml_doc_target_kind(
          ryspec_toml_doc_resolve(doc, NULL, e->u.s, (size_t)e->u.str.len)) ==
          RYSPEC_ENTITY_NAMESPACE) {
        return ryspec_lint_fail(
          diag,
          e,
          "output `%s` of monitor `%.*s` names a namespace",
          e->u.s,
          m.len,
          m.name);
      }
    }
  }
  return ryspec_lint_walk_all(doc, names_no_namespace, (void*)doc, diag);
}

/* ===========================================================================
 * The checks that follow names into their definitions: Rules 12 and 13. */

/* The rule positions a reference written at at leads to, each passed to fn
 * until it returns anything but RYSPEC_OK: a rule's, or a property's
 * `given` and `check`, or with check_only its `check` alone. */
typedef int (*position_fn)(
  const ryspec_toml_doc_entity* position, void* ctx, ryspec_diag* diag);

static int each_definition(
  const ryspec_toml_doc* doc,
  const ryspec_toml_doc_entity* at,
  const toml_datum_t* name,
  bool check_only,
  position_fn fn,
  void* ctx,
  ryspec_diag* diag)
{
  ryspec_toml_doc_target t =
    ryspec_toml_doc_resolve(doc, at, name->u.str.ptr, (size_t)name->u.str.len);
  ryspec_toml_doc_entity part;
  switch(ryspec_toml_doc_target_kind(t)) {
    case RYSPEC_ENTITY_RULE:
    case RYSPEC_ENTITY_PRIVATE_RULE:
      return fn(&t.entity, ctx, diag);
    case RYSPEC_ENTITY_PROPERTY:
      if(
        !check_only &&
        ryspec_toml_doc_property_part(&t.entity, RYSPEC_ENTITY_GIVEN, &part)) {
        int s = fn(&part, ctx, diag);
        if(s != RYSPEC_OK) {
          return s;
        }
      }
      return ryspec_toml_doc_property_part(
               &t.entity, RYSPEC_ENTITY_CHECK, &part)
               ? fn(&part, ctx, diag)
               : RYSPEC_OK;
    default:
      return RYSPEC_OK;
  }
}

/* Per rule position, by its rule's value: whether it is not yet visited,
 * being visited, or visited, and a byte a check keeps for it; in a table of
 * open addressing sized for twice the document's positions. A slot past the
 * table, never reached, takes what finds no room. */
enum { UNSEEN, OPEN, DONE };

typedef struct marks {
  const toml_datum_t** keys;
  unsigned char* states;
  unsigned char* values;
  size_t cap;
} marks;

static int count_position(
  const ryspec_toml_doc_entity* e, void* ctx, ryspec_diag* diag)
{
  (void)diag;
  *(size_t*)ctx += ryspec_toml_doc_is_position(e->kind);
  return RYSPEC_OK;
}

static void marks_free(marks* m)
{
  void (*mem_free)(void*) = ryspec_toml_options().mem_free;
  mem_free(m->keys);
  mem_free(m->states);
  mem_free(m->values);
}

/* Marks for doc's positions, every one UNSEEN, returning whether they could
 * be allocated; if not, m is only fit to be freed. */
static bool marks_init(const ryspec_toml_doc* doc, marks* m)
{
  size_t n = 0;
  ryspec_toml_doc_each_entity(doc, count_position, &n, NULL);
  size_t cap = 8;
  while(cap < 2 * n) {
    cap *= 2;
  }
  void* (*mem_realloc)(void*, size_t) = ryspec_toml_options().mem_realloc;
  *m = (marks){mem_realloc(NULL, (cap + 1) * sizeof *m->keys),
               mem_realloc(NULL, cap + 1),
               mem_realloc(NULL, cap + 1),
               cap};
  if(!m->keys || !m->states || !m->values) {
    return false;
  }
  for(size_t i = 0; i <= cap; i++) {
    m->keys[i] = NULL;
  }
  memset(m->states, UNSEEN, cap + 1);
  memset(m->values, 0, cap + 1);
  return true;
}

/* The slot of the position whose rule is v, taken for it where it has
 * none. */
static size_t mark(marks* m, const toml_datum_t* v)
{
  size_t i = (size_t)(((uint64_t)(uintptr_t)v * 0x9E3779B97F4A7C15u) >> 32) &
             (m->cap - 1);
  for(size_t probes = 0; probes < m->cap;
      probes++, i = (i + 1) & (m->cap - 1)) {
    if(m->keys[i] == v || !m->keys[i]) {
      m->keys[i] = v;
      return i;
    }
  }
  return m->cap;
}

static int out_of_memory(ryspec_diag* diag)
{
  ryspec_diagnose(diag, RYSPEC_ERROR_MEMORY, 0, 0, "out of memory");
  return RYSPEC_ERROR_MEMORY;
}

/* ---------------------------------------------------------------------------
 * Rule 13: no rule is defined in terms of itself, directly or around a ring
 * of names. A property named for its verdict is defined by its `given` and
 * its `check`. Reported at the reference that closes the ring. */

typedef struct ring {
  const ryspec_toml_doc* doc;
  marks marks;
  const toml_datum_t* at; /* the reference followed */
} ring;

static int visit(
  const ryspec_toml_doc_entity* position,
  size_t slot,
  ring* r,
  ryspec_diag* diag);

/* A `given` or `check` carries its property's name. */
static int step(
  const ryspec_toml_doc_entity* position, void* ctx, ryspec_diag* diag)
{
  ring* r = ctx;
  size_t slot = mark(&r->marks, position->value);
  if(r->marks.states[slot] == OPEN) {
    return ryspec_lint_fail(
      diag,
      r->at,
      "`%.*s` is defined in terms of itself",
      position->len,
      position->name);
  }
  return r->marks.states[slot] == UNSEEN ? visit(position, slot, r, diag)
                                         : RYSPEC_OK;
}

static int follow(const ryspec_lint_node* n, void* ctx, ryspec_diag* diag)
{
  ring* r = ctx;
  if(n->kind != RYSPEC_LINT_REFERENCE) {
    return RYSPEC_OK;
  }
  r->at = n->at;
  return each_definition(r->doc, n->position, n->datum, false, step, r, diag);
}

static int visit(
  const ryspec_toml_doc_entity* position,
  size_t slot,
  ring* r,
  ryspec_diag* diag)
{
  r->marks.states[slot] = OPEN;
  int s = ryspec_lint_walk_rule(position, follow, r, diag);
  r->marks.states[slot] = DONE;
  return s;
}

static int visit_unseen(
  const ryspec_toml_doc_entity* e, void* ctx, ryspec_diag* diag)
{
  ring* r = ctx;
  if(!ryspec_toml_doc_is_position(e->kind)) {
    return RYSPEC_OK;
  }
  size_t slot = mark(&r->marks, e->value);
  return r->marks.states[slot] == UNSEEN ? visit(e, slot, r, diag) : RYSPEC_OK;
}

int ryspec_lint_check_no_cycle(const ryspec_toml_doc* doc, ryspec_diag* diag)
{
  ring r = {doc, {0}, NULL};
  int s = marks_init(doc, &r.marks)
            ? ryspec_toml_doc_each_entity(doc, visit_unseen, &r, diag)
            : out_of_memory(diag);
  marks_free(&r.marks);
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

static const char* cone_name(cone c)
{
  return c == CONE_PAST ? "past" : "future";
}

static cone cone_of_op(ryspec_rule_op op)
{
  switch(op) {
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
  const ryspec_toml_doc* doc;
  marks marks; /* a position's value its cone, once DONE */
  cone found;  /* what each_definition() last led to */
} cones;

static int cone_of(
  cones* c,
  const toml_datum_t* v,
  const ryspec_toml_doc_entity* at,
  cone* out,
  ryspec_diag* diag);

static int position_cone(
  const ryspec_toml_doc_entity* position, void* ctx, ryspec_diag* diag)
{
  cones* c = ctx;
  size_t slot = mark(&c->marks, position->value);
  if(c->marks.states[slot] == UNSEEN) {
    c->marks.states[slot] = OPEN;
    cone own;
    int s = cone_of(c, position->value, position, &own, diag);
    c->marks.values[slot] = (unsigned char)own;
    c->marks.states[slot] = DONE;
    if(s != RYSPEC_OK) {
      return s;
    }
  }
  c->found =
    c->marks.states[slot] == DONE ? (cone)c->marks.values[slot] : CONE_NEUTRAL;
  return RYSPEC_OK;
}

/* How a diagnostic names the i-th operand of the n rule operands of v. */
static void describe(
  const toml_datum_t* e, int i, int n, char* buf, size_t size)
{
  if(e->type == TOML_STRING) {
    snprintf(buf, size, "'%s'", e->u.s);
  }
  else if(n == 1) {
    snprintf(buf, size, "its operand");
  }
  else {
    snprintf(buf, size, "operand %d", i);
  }
}

static int cone_of(
  cones* c,
  const toml_datum_t* v,
  const ryspec_toml_doc_entity* at,
  cone* out,
  ryspec_diag* diag)
{
  *out = CONE_NEUTRAL;
  if(v->type == TOML_STRING) {
    c->found = CONE_NEUTRAL;
    int s = each_definition(c->doc, at, v, true, position_cone, c, diag);
    *out = c->found;
    return s;
  }
  if(v->type != TOML_ARRAY || v->u.arr.size == 0) {
    return RYSPEC_OK;
  }
  const toml_datum_t* head = &v->u.arr.elem[0];
  ryspec_rule_op op = RYSPEC_RULE_OP_NOT;
  bool known =
    head->type == TOML_STRING && ryspec_rule_op_from_name(head->u.s, &op);
  if(
    known &&
    (ryspec_lint_op_is_comparison(op) || op == RYSPEC_RULE_OP_ASSIGN)) {
    return RYSPEC_OK; /* names of values: no direction */
  }
  cone own = known ? cone_of_op(op) : CONE_NEUTRAL;
  const char* op_name = known ? ryspec_rule_op_name(op) : head->u.s;
  int operands = 0;
  for(int i = 1; i < v->u.arr.size; i++) {
    operands += v->u.arr.elem[i].type != TOML_TABLE;
  }
  cone shared = CONE_NEUTRAL;
  const toml_datum_t* first = NULL;
  int first_i = 0;
  for(int i = 1, k = 0; i < v->u.arr.size; i++) {
    const toml_datum_t* e = &v->u.arr.elem[i];
    if(e->type == TOML_TABLE) {
      continue; /* a bound or a binding */
    }
    k++;
    cone oc;
    int s = cone_of(c, e, at, &oc, diag);
    if(s != RYSPEC_OK) {
      return s;
    }
    if(oc == CONE_NEUTRAL) {
      continue;
    }
    char what[160];
    describe(e, k, operands, what, sizeof what);
    const toml_datum_t* place = e->lineno ? e : v;
    if(own != CONE_NEUTRAL) {
      if(oc != own) {
        return ryspec_lint_fail(
          diag,
          place,
          "'%s' looks to the %s and %s is a %s-time rule",
          op_name,
          cone_name(own),
          what,
          cone_name(oc));
      }
      continue;
    }
    if(shared == CONE_NEUTRAL) {
      shared = oc;
      first = e;
      first_i = k;
    }
    else if(oc != shared) {
      char first_what[160];
      describe(first, first_i, operands, first_what, sizeof first_what);
      return ryspec_lint_fail(
        diag,
        place,
        "in '%s', %s is a %s-time rule and the operand at "
        "position %d is a %s-time rule",
        op_name,
        first_what,
        cone_name(shared),
        k,
        cone_name(oc));
    }
  }
  *out = own != CONE_NEUTRAL ? own : shared;
  return RYSPEC_OK;
}

static int each_cone(
  const ryspec_toml_doc_entity* e, void* ctx, ryspec_diag* diag)
{
  return ryspec_toml_doc_is_position(e->kind) ? position_cone(e, ctx, diag)
                                              : RYSPEC_OK;
}

int ryspec_lint_check_one_cone(const ryspec_toml_doc* doc, ryspec_diag* diag)
{
  cones c = {doc, {0}, CONE_NEUTRAL};
  int s = marks_init(doc, &c.marks)
            ? ryspec_toml_doc_each_entity(doc, each_cone, &c, diag)
            : out_of_memory(diag);
  marks_free(&c.marks);
  return s;
}

/* ===========================================================================
 * The checks of quantifiers: Rules 14, 15, 16 and 17.
 *
 * One walk serves the four, carrying the names each enclosing quantifier
 * binds, and reports the violations of the one rule it is asked for. A use
 * marks every binding of its name, so a name rebound (Rule 15) leaves no
 * binding unused (Rule 17) the rebinding has not. */

/* A name a quantifier binds, the innermost first. */
typedef struct scope {
  const toml_datum_t* name;
  bool used;
  struct scope* up;
} scope;

typedef struct quantifiers {
  const ryspec_toml_doc* doc;
  const ryspec_toml_doc_entity* at;
  int rule;
} quantifiers;

static bool same_name(const toml_datum_t* a, const char* b, size_t len)
{
  return (size_t)a->u.str.len == len && memcmp(a->u.str.ptr, b, len) == 0;
}

static int walk(
  const quantifiers* q, const toml_datum_t* v, scope* bound, ryspec_diag* diag);

/* The quantifier v, binding the names of qvars from the i-th on, each
 * pushed onto bound as a frame, then walking its rule operands. */
static int bind(
  const quantifiers* q,
  const toml_datum_t* v,
  const toml_datum_t* qvars,
  int i,
  scope* bound,
  ryspec_diag* diag)
{
  if(i == qvars->u.arr.size) {
    for(int k = 1; k < v->u.arr.size; k++) {
      int s = walk(q, &v->u.arr.elem[k], bound, diag);
      if(s != RYSPEC_OK) {
        return s;
      }
    }
    return RYSPEC_OK;
  }
  const toml_datum_t* name = &qvars->u.arr.elem[i];
  if(name->type != TOML_STRING) {
    return bind(q, v, qvars, i + 1, bound, diag);
  }
  const char* s = name->u.str.ptr;
  size_t len = (size_t)name->u.str.len;
  if(q->rule == RYSPEC_LINT_QVAR_NO_REBIND) {
    for(const scope* b = bound; b; b = b->up) {
      if(same_name(b->name, s, len)) {
        return ryspec_lint_fail(
          diag,
          placed(name, v),
          "`%s` is already bound by an enclosing quantifier",
          s);
      }
    }
  }
  if(
    q->rule == RYSPEC_LINT_QVAR_NO_SHADOW &&
    ryspec_toml_doc_resolve(q->doc, q->at, s, len).resolution !=
      RYSPEC_RESOLVES_DEDUCED) {
    return ryspec_lint_fail(
      diag, placed(name, v), "quantified name `%s` shadows a declared name", s);
  }
  scope frame = {name, false, bound};
  int st = bind(q, v, qvars, i + 1, &frame, diag);
  if(st == RYSPEC_OK && q->rule == RYSPEC_LINT_QVAR_USED && !frame.used) {
    return ryspec_lint_fail(
      diag, placed(name, v), "`%s` is never used in the rule it quantifies", s);
  }
  return st;
}

/* assign's quantified variable, { qvar = "a" }: a use of a. */
static int use(
  const quantifiers* q,
  const toml_datum_t* t,
  scope* bound,
  const toml_datum_t* v,
  ryspec_diag* diag)
{
  const toml_datum_t* name = ryspec_toml_value_lookup(t, "qvar", 4);
  if(!name || name->type != TOML_STRING) {
    return RYSPEC_OK;
  }
  bool found = false;
  for(scope* b = bound; b; b = b->up) {
    if(same_name(b->name, name->u.str.ptr, (size_t)name->u.str.len)) {
      b->used = true;
      found = true;
    }
  }
  if(!found && q->rule == RYSPEC_LINT_QVAR_BOUND) {
    return ryspec_lint_fail(
      diag,
      placed(t, v),
      "`%s` is a quantified variable, and an enclosing "
      "quantifier binds no such name",
      name->u.s);
  }
  return RYSPEC_OK;
}

static int walk(
  const quantifiers* q, const toml_datum_t* v, scope* bound, ryspec_diag* diag)
{
  if(v->type != TOML_ARRAY || v->u.arr.size == 0) {
    return RYSPEC_OK;
  }
  const toml_datum_t* head = &v->u.arr.elem[0];
  ryspec_rule_op op = RYSPEC_RULE_OP_NOT;
  bool known =
    head->type == TOML_STRING && ryspec_rule_op_from_name(head->u.s, &op);
  if(known && (op == RYSPEC_RULE_OP_FORALL || op == RYSPEC_RULE_OP_EXISTS)) {
    const toml_datum_t* last = &v->u.arr.elem[v->u.arr.size - 1];
    const toml_datum_t* qvars = ryspec_toml_value_lookup(last, "qvars", 5);
    if(qvars && qvars->type == TOML_ARRAY) {
      return bind(q, v, qvars, 0, bound, diag);
    }
  }
  for(int i = 1; i < v->u.arr.size; i++) {
    const toml_datum_t* e = &v->u.arr.elem[i];
    int s = e->type == TOML_TABLE ? (known && op == RYSPEC_RULE_OP_ASSIGN
                                       ? use(q, e, bound, v, diag)
                                       : RYSPEC_OK)
                                  : walk(q, e, bound, diag);
    if(s != RYSPEC_OK) {
      return s;
    }
  }
  return RYSPEC_OK;
}

static int check_position(
  const ryspec_toml_doc_entity* e, void* ctx, ryspec_diag* diag)
{
  if(!ryspec_toml_doc_is_position(e->kind)) {
    return RYSPEC_OK;
  }
  quantifiers q = *(const quantifiers*)ctx;
  q.at = e;
  return walk(&q, e->value, NULL, diag);
}

static int check(const ryspec_toml_doc* doc, int rule, ryspec_diag* diag)
{
  quantifiers q = {doc, NULL, rule};
  return ryspec_toml_doc_each_entity(doc, check_position, &q, diag);
}

/* Rule 14: a quantified variable is bound by an enclosing quantifier. */
int ryspec_lint_check_qvar_bound(const ryspec_toml_doc* doc, ryspec_diag* diag)
{
  return check(doc, RYSPEC_LINT_QVAR_BOUND, diag);
}

/* Rule 15: a nested quantifier does not rebind a name an enclosing one
 * binds. */
int ryspec_lint_check_qvar_no_rebind(
  const ryspec_toml_doc* doc, ryspec_diag* diag)
{
  return check(doc, RYSPEC_LINT_QVAR_NO_REBIND, diag);
}

/* Rule 16: a quantified name does not reuse the name of a variable, a rule
 * or a property, as seen from where it is bound. */
int ryspec_lint_check_qvar_no_shadow(
  const ryspec_toml_doc* doc, ryspec_diag* diag)
{
  return check(doc, RYSPEC_LINT_QVAR_NO_SHADOW, diag);
}

/* Rule 17: every name a quantifier binds is used by the rule it
 * quantifies. */
int ryspec_lint_check_qvar_used(const ryspec_toml_doc* doc, ryspec_diag* diag)
{
  return check(doc, RYSPEC_LINT_QVAR_USED, diag);
}
