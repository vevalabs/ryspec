/* Tests of rules: expressions against their prefix twins, the binding of
 * their operators, their bounds, their errors, and rules found in a
 * document. They take no argument, and run alike on every library. */
#include "ryspec/ryspec.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "toml_doc.h"
#include "expr_parser.h"

static int failures;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
      failures++;                                                              \
    }                                                                          \
  } while (0)

/* A document whose one rule, r, is the expression src, which starts at
 * line 3, column 8. */
static ryspec_toml_doc *parse_rule(const char *src, ryspec_diagnostic *diag) {
  char buf[8192];
  int n = snprintf(buf, sizeof buf,
                   "version = \"0\"\n[rules]\nr = '''%s'''\n", src);
  return ryspec_toml_parse(buf, (size_t)n, "<rule>", diag);
}

/* The value of a table's key, or NULL. */
static const toml_datum_t *get(const toml_datum_t *d, const char *key) {
  return ryspec_toml_value_lookup(d, key, strlen(key));
}

/* An array's i-th element, or NULL. */
static const toml_datum_t *element(const toml_datum_t *d, int i) {
  return d && d->type == TOML_ARRAY && i < d->u.arr.size ? &d->u.arr.elem[i]
                                                          : NULL;
}

/* The value at the keys that follow, to a NULL, from the root of doc. */
static const toml_datum_t *dig(const ryspec_toml_doc *doc, ...) {
  const toml_datum_t *v = &doc->result.toptab;
  va_list ap;
  va_start(ap, doc);
  for (const char *key; (key = va_arg(ap, const char *));) {
    v = get(v, key);
  }
  va_end(ap);
  return v;
}

/* The rule r of doc. */
static const toml_datum_t *rule_of(const ryspec_toml_doc *doc) {
  return dig(doc, "rules", "r", NULL);
}

/* The document of the expression src, or NULL, reported. */
static ryspec_toml_doc *parse(const char *src) {
  ryspec_diagnostic diag;
  ryspec_toml_doc *doc = parse_rule(src, &diag);
  if (!doc) {
    fprintf(stderr, "%s: %d:%d: %s\n", src, diag.line, diag.column,
            diag.message);
  }
  CHECK(diag.status == (doc ? RYSPEC_OK : RYSPEC_ERROR_GRAMMAR));
  return doc;
}

/* Whether a and b are one value: of one type, and alike in every part, a
 * table's keys in one order. No rule holds a date or a time. */
static bool same(const toml_datum_t *a, const toml_datum_t *b) {
  if (!a || !b || a->type != b->type) {
    return false;
  }
  switch (a->type) {
  case TOML_STRING:
    return a->u.str.len == b->u.str.len &&
           memcmp(a->u.str.ptr, b->u.str.ptr, (size_t)a->u.str.len) == 0;
  case TOML_INT64:
    return a->u.int64 == b->u.int64;
  case TOML_FP64:
    return a->u.fp64 == b->u.fp64;
  case TOML_BOOLEAN:
    return a->u.boolean == b->u.boolean;
  case TOML_ARRAY:
    if (a->u.arr.size != b->u.arr.size) {
      return false;
    }
    for (int i = 0; i < a->u.arr.size; i++) {
      if (!same(&a->u.arr.elem[i], &b->u.arr.elem[i])) {
        return false;
      }
    }
    return true;
  case TOML_TABLE:
    if (a->u.tab.size != b->u.tab.size) {
      return false;
    }
    for (int i = 0; i < a->u.tab.size; i++) {
      if (a->u.tab.len[i] != b->u.tab.len[i] ||
          memcmp(a->u.tab.key[i], b->u.tab.key[i], (size_t)a->u.tab.len[i]) ||
          !same(&a->u.tab.value[i], &b->u.tab.value[i])) {
        return false;
      }
    }
    return true;
  default:
    return false;
  }
}

/* Whether v is the value the TOML text prefix spells. */
static bool is(const toml_datum_t *v, const char *prefix) {
  char src[2048];
  int n = snprintf(src, sizeof src, "v = %s\n", prefix);
  toml_result_t r = toml_parse(src, n);
  if (!r.ok) {
    fprintf(stderr, "%s: %s\n", prefix, r.errmsg);
  }
  bool ok = r.ok && same(v, get(&r.toptab, "v"));
  toml_free(r);
  return ok;
}

/* src parses, into the rule prefix spells. */
static void expect(const char *src, const char *prefix) {
  ryspec_toml_doc *doc = parse(src);
  if (!doc) {
    failures++;
    return;
  }
  if (!is(rule_of(doc), prefix)) {
    fprintf(stderr, "%s\n  does not give %s\n", src, prefix);
    failures++;
  }
  ryspec_toml_doc_free(doc);
}

/* src does not parse, and the fault is at line and column of src. */
static void expect_error(const char *src, int line, int column) {
  ryspec_diagnostic diag;
  ryspec_toml_doc *doc = parse_rule(src, &diag);
  /* In the document: src starts at line 3, column 8. */
  if (line == 1) {
    column += 7;
  }
  line += 2;
  CHECK(doc == NULL);
  if (doc) {
    fprintf(stderr, "%s: parsed\n", src);
    ryspec_toml_doc_free(doc);
    return;
  }
  CHECK(diag.status == RYSPEC_ERROR_GRAMMAR);
  CHECK(diag.message[0] != '\0');
  if (diag.line != line || diag.column != column) {
    fprintf(stderr, "%s: fault at %d:%d, not %d:%d: %s\n", src, diag.line,
            diag.column, line, column, diag.message);
    failures++;
  }
}

static bool at(const toml_datum_t *v, int line, int column) {
  return v && v->lineno == line && v->colno == column;
}

/* Every row of SPEC.md's table of twins, in expression form, against the
 * prefix it means, both read from one document: the expression is replaced
 * by its twin as the document is parsed. */
static void test_twins(void) {
  static const char *const twins[][2] = {
      {"({p})", "\"p\""},
      {"({speed < 30})", "[\"lt\", \"speed\", { value = 30 }]"},
      {"({a == b})", "[\"eq\", \"a\", \"b\"]"},
      {"({s := sensor_id})", "[\"assign\", \"sensor_id\", { qvar = \"s\" }]"},
      {"(not {e})", "[\"not\", \"e\"]"},
      {"(next {e})", "[\"next\", \"e\"]"},
      {"(once[2:5:s] {e})",
       "[\"once\", \"e\", { min = 2, max = 5, time_unit = \"s\" }]"},
      {"({e} since[:5] {f})", "[\"since\", \"e\", \"f\", { max = 5 }]"},
      {"({e} and {f} and {g})", "[\"and\", \"e\", \"f\", \"g\"]"},
      {"({e} -> {f})", "[\"implies\", \"e\", \"f\"]"},
      {"({e} implies {f})", "[\"implies\", \"e\", \"f\"]"},
      {"(forall[a, b] {e})", "[\"forall\", \"e\", { qvars = [\"a\", \"b\"] }]"},
      {"(forall[s] (({s := sensor_id}) -> {reading < 120}))",
       "[\"forall\", [\"implies\", [\"assign\", \"sensor_id\", { qvar = \"s\" "
       "}], [\"lt\", \"reading\", { value = 120 }]], { qvars = [\"s\"] }]"},
  };
  size_t n = sizeof twins / sizeof twins[0];
  char doc_src[8192] = "version = \"0\"\n[rules]\n";
  for (size_t i = 0; i < n; i++) {
    char line[512];
    snprintf(line, sizeof line, "e%zu = '%s'\np%zu = %s\n", i, twins[i][0], i,
             twins[i][1]);
    strcat(doc_src, line);
  }
  ryspec_diagnostic diag;
  ryspec_toml_doc *doc =
      ryspec_toml_parse(doc_src, strlen(doc_src), "<twins>", &diag);
  CHECK(doc != NULL);
  if (!doc) {
    fprintf(stderr, "%d:%d: %s\n", diag.line, diag.column, diag.message);
    return;
  }
  for (size_t i = 0; i < n; i++) {
    char e[16], p[16];
    snprintf(e, sizeof e, "e%zu", i);
    snprintf(p, sizeof p, "p%zu", i);
    if (!same(dig(doc, "rules", e, NULL), dig(doc, "rules", p, NULL))) {
      fprintf(stderr, "%s does not give its twin %s\n", twins[i][0],
              twins[i][1]);
      failures++;
    }
  }
  ryspec_toml_doc_free(doc);
}

static void test_binding(void) {
  /* Tightest to loosest: not/next, unary temporal, since/until, and,
   * or/xor, ->/implies, quantifiers. */
  expect("(not {a} and {b})", "[\"and\", [\"not\", \"a\"], \"b\"]");
  expect("(once {a} since {b})",
         "[\"since\", [\"once\", \"a\"], \"b\"]");
  expect("({a} since {b} and {c})",
         "[\"and\", [\"since\", \"a\", \"b\"], \"c\"]");
  expect("({a} and {b} or {c})", "[\"or\", [\"and\", \"a\", \"b\"], \"c\"]");
  expect("({a} or {b} -> {c})",
         "[\"implies\", [\"or\", \"a\", \"b\"], \"c\"]");
  expect("(forall[s] {a} -> {b})",
         "[\"forall\", [\"implies\", \"a\", \"b\"], { qvars = [\"s\"] }]");
  expect("(not not {a})", "[\"not\", [\"not\", \"a\"]]");
  expect("(once[1:] always[:2] {a})",
         "[\"once\", [\"always\", \"a\", { max = 2 }], { min = 1 }]");

  /* since and until are binary, to the left; the n-ary chains gather. */
  expect("({a} since {b} until {c})",
         "[\"until\", [\"since\", \"a\", \"b\"], \"c\"]");
  expect("({a} or {b} or {c})", "[\"or\", \"a\", \"b\", \"c\"]");
  expect("({a} xor {b} xor {c})", "[\"xor\", \"a\", \"b\", \"c\"]");
  expect("({a} or {b} xor {c})", "[\"xor\", [\"or\", \"a\", \"b\"], \"c\"]");
  expect("({a} -> {b} implies {c})", "[\"implies\", \"a\", \"b\", \"c\"]");

  /* A parenthesised operand ends a chain. */
  expect("(({a} and {b}) and {c})",
         "[\"and\", [\"and\", \"a\", \"b\"], \"c\"]");
  expect("({a} and ({b} and {c}))",
         "[\"and\", \"a\", [\"and\", \"b\", \"c\"]]");
  expect("(({a} -> {b}) -> {c})",
         "[\"implies\", [\"implies\", \"a\", \"b\"], \"c\"]");
  expect("((({a})))", "\"a\"");
  expect("(exists[a] forall[b] {x})",
         "[\"exists\", [\"forall\", \"x\", { qvars = [\"b\"] }], { qvars = "
         "[\"a\"] }]");
}

static void test_atoms(void) {
  expect("({a <= -1.5})", "[\"le\", \"a\", { value = -1.5 }]");
  expect("({a >= +2e3})", "[\"ge\", \"a\", { value = 2000 }]");
  expect("({a > 0.25})", "[\"gt\", \"a\", { value = 0.25 }]");
  expect("({a != b})", "[\"ne\", \"a\", \"b\"]");
  expect("({a == 7})", "[\"eq\", \"a\", { value = 7 }]");
  /* A word is a keyword only where the grammar has one. */
  expect("({once})", "\"once\"");
  expect("({not < and})", "[\"lt\", \"not\", \"and\"]");
  expect("(forall[always] {always := always})",
         "[\"forall\", [\"assign\", \"always\", { qvar = \"always\" }], { "
         "qvars = [\"always\"] }]");
  expect("( \t\n{ a }\n)", "\"a\"");
}

static void test_bounds(void) {
  /* Position alone says what a token fills. */
  expect("(once[:s] {p})", "[\"once\", \"p\", { max = \"s\" }]");
  expect("(once[1::s] {p})",
         "[\"once\", \"p\", { min = 1, time_unit = \"s\" }]");
  expect("(once[min:max:min] {p})",
         "[\"once\", \"p\", { min = \"min\", max = \"max\", time_unit = "
         "\"min\" }]");
  expect("(eventually[0.5:2:ms] {p})",
         "[\"eventually\", \"p\", { min = 0.5, max = 2, time_unit = \"ms\" }]");
  expect("({p} until[:5:d] {q})",
         "[\"until\", \"p\", \"q\", { max = 5, time_unit = \"d\" }]");
  for (const char *const *unit = (const char *const[]){"ns", "us", "ms", "s",
                                                        "min", "h", "d", NULL};
       *unit; unit++) {
    char src[64];
    snprintf(src, sizeof src, "(once[1:2:%s] {p})", *unit);
    ryspec_toml_doc *doc = parse(src);
    const toml_datum_t *unit_of = get(element(rule_of(doc), 2), "time_unit");
    CHECK(unit_of && unit_of->type == TOML_STRING &&
          strcmp(unit_of->u.s, *unit) == 0);
    ryspec_toml_doc_free(doc);
  }

  ryspec_toml_doc *doc = parse("(historically[2:limit] {p})");
  const toml_datum_t *rule = rule_of(doc);
  CHECK(rule->type == TOML_ARRAY);
  CHECK(rule->u.arr.size == 3);
  CHECK(is(element(rule, 0), "\"historically\""));
  CHECK(is(element(rule, 1), "\"p\""));
  const toml_datum_t *bound = element(rule, 2);
  const toml_datum_t *min = get(bound, "min");
  CHECK(min && min->type == TOML_INT64 && min->u.int64 == 2);
  CHECK(is(get(bound, "max"), "\"limit\""));
  CHECK(get(bound, "time_unit") == NULL);
  ryspec_toml_doc_free(doc);
}

static void test_errors(void) {
  expect_error("(once[3:10])", 1, 12);
  expect_error("(once[3] {p})", 1, 8);
  expect_error("(once[:] {p})", 1, 8);
  expect_error("(once[::s] {p})", 1, 8);
  expect_error("(once[3:10s] {p})", 1, 11);
  expect_error("(once[3:10:sec] {p})", 1, 12);
  expect_error("(once[1:2:s:ms] {p})", 1, 12);
  expect_error("(next[3] {p})", 1, 6);
  expect_error("(prev {p})", 1, 2);
  expect_error("({p} equiv {q})", 1, 6);
  expect_error("(p)", 1, 2);
  expect_error("({p} since)", 1, 11);
  expect_error("(until {p} {q})", 1, 2);
  expect_error("(forall[] {p})", 1, 9);
  expect_error("(forall {s} {p})", 1, 9);
  expect_error("({s = x})", 1, 5);
  expect_error("({s := 3})", 1, 8);
  expect_error("({a < (b)})", 1, 7);
  expect_error("({a} and forall[s] {b})", 1, 10);
  expect_error("(not forall[s] {b})", 1, 6);
  expect_error("({a.b})", 1, 4);
  expect_error("({a < \"x\"})", 1, 7);
  expect_error("({a})({b})", 1, 6);
  expect_error("({a}", 1, 5);
  expect_error("(\n  {a} and\n  )", 3, 3);
  expect_error("({a} @ {b})", 1, 6);
  /* A sign stands against its digits. */
  expect_error("({a < - 3})", 1, 7);
  expect_error("({a < ->})", 1, 7);

  expect_error("(", 1, 2);
}

/* What nesting the expression parser takes, prefix form takes too, up to
 * what tomlc17 holds of an array's nesting; past it, the expression is
 * refused as TOML past its limit, placed at the expression. */
static void test_depth(void) {
  char src[4096];
  size_t n = 0;
  src[n++] = '(';
  for (int i = 0; i < 25; i++) {
    n += (size_t)snprintf(src + n, sizeof src - n, "not ");
  }
  snprintf(src + n, sizeof src - n, "{p})");
  ryspec_toml_doc *doc = parse(src);
  CHECK(doc != NULL);
  ryspec_toml_doc_free(doc);

  ryspec_diagnostic diag;
  n = (size_t)snprintf(src, sizeof src,
                       "version = \"0\"\n[rules]\nok = \"({p})\"\nr = \"(");
  for (int i = 0; i < 100; i++) {
    n += (size_t)snprintf(src + n, sizeof src - n, "not ");
  }
  snprintf(src + n, sizeof src - n, "{p})\"\n");
  CHECK(ryspec_toml_parse(src, strlen(src), "<deep>", &diag) == NULL);
  CHECK(diag.status == RYSPEC_ERROR_TOML_LIMIT);
  CHECK(diag.line == 4 && diag.column == 6);
}

static void test_document_rules(void) {
  const char *src = "version = \"0\"\n"
                    "[rules]\n"
                    "r = \"(once {p})\"\n"
                    "malformed = [\"once\"]\n"
                    "[properties.q]\n"
                    "check = [\"not\", \"r\"]\n"
                    "[properties.q.where]\n"
                    "w = \"({x} and {y})\"\n"
                    "[namespace.a.b.rules]\n"
                    "deep = [\"and\", \"x\", \"a.b.y\"]\n"
                    "[namespace.a.b.properties.prop]\n"
                    "check = '''\n"
                    "({p}\n"
                    "   and {q})'''\n"
                    "given = \"(next {p})\"\n";
  ryspec_diagnostic diag;
  ryspec_toml_doc *doc = ryspec_toml_parse(src, strlen(src), "<test>", &diag);
  CHECK(doc != NULL);
  if (!doc) {
    fprintf(stderr, "%d:%d: %s\n", diag.line, diag.column, diag.message);
    return;
  }
  const toml_datum_t *rule = dig(doc, "rules", "r", NULL);
  CHECK(is(rule, "[\"once\", \"p\"]"));
  /* Placed in the document: the string starts at 3:6. */
  CHECK(at(rule, 3, 7));
  CHECK(at(element(rule, 0), 3, 7));
  CHECK(at(element(rule, 1), 3, 12));

  /* A private rule is translated too. */
  CHECK(is(dig(doc, "properties", "q", "where", "w", NULL),
           "[\"and\", \"x\", \"y\"]"));

  /* A prefix rule is read as written, malformed or not. */
  CHECK(is(dig(doc, "rules", "malformed", NULL), "[\"once\"]"));
  CHECK(is(dig(doc, "properties", "q", "check", NULL), "[\"not\", \"r\"]"));
  CHECK(is(element(dig(doc, "namespace", "a", "b", "rules", "deep", NULL), 2),
           "\"a.b.y\""));

  /* In a namespace, a property's `check` and `given`, placed in the
   * document. */
  rule = dig(doc, "namespace", "a", "b", "properties", "prop", "check", NULL);
  CHECK(at(element(rule, 2), 14, 8));
  rule = dig(doc, "namespace", "a", "b", "properties", "prop", "given", NULL);
  CHECK(is(rule, "[\"next\", \"p\"]"));
  CHECK(at(rule, 15, 11));
  ryspec_toml_doc_free(doc);

  /* An expression that does not parse refuses the document, placed in it. */
  src = "version = \"0\"\n"
        "[rules]\n"
        "good = \"({p})\"\n"
        "bad = \"(once)\"\n";
  CHECK(ryspec_toml_parse(src, strlen(src), "<test>", &diag) == NULL);
  CHECK(diag.status == RYSPEC_ERROR_GRAMMAR);
  CHECK(diag.line == 4 && diag.column == 13);
}

/* The private table of operators (expr_parser.h). */
static void test_ops(void) {
  ryspec_rule_op op;
  for (int i = RYSPEC_RULE_OP_NOT; i <= RYSPEC_RULE_OP_EXISTS; i++) {
    CHECK(ryspec_rule_op_from_name(ryspec_rule_op_name((ryspec_rule_op)i),
                                   &op));
    CHECK(op == (ryspec_rule_op)i);
  }
  CHECK(!ryspec_rule_op_from_name("nand", &op));
  CHECK(strcmp(ryspec_rule_op_name(RYSPEC_RULE_OP_STARTSWITH),
               "startswith") == 0);
}

int main(void) {
  test_twins();
  test_binding();
  test_atoms();
  test_bounds();
  test_errors();
  test_depth();
  test_document_rules();
  test_ops();
  if (failures) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  return 0;
}
