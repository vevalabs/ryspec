/* Tests of a document's entities, read from its tree: the visitor, which
 * meets them in the document's order, the tables holding each, and the
 * resolution of names over them, values of the wrong shape among what they
 * are given. They take no argument. */
#include <stdio.h>
#include <string.h>

#include "ryspec/ryspec.h"

#include "toml_doc.h"

static int failures;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if(!(cond)) {                                                              \
      fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
      failures++;                                                              \
    }                                                                          \
  } while(0)

/* A document of every kind of entity, a namespace two deep among them, and
 * a monitor sharing its name with a variable. Its entities, in the order
 * the visitor meets them: */
static const char document[] =
  "version = \"0\"\n"
  "[rules]\n"
  "top = \"x\"\n"
  "[properties.p]\n"
  "given = \"top\"\n"
  "check = \"v\"\n"
  "[properties.p.where]\n"
  "w = \"top\"\n"
  "[namespace.a.b.rules]\n"
  "r = \"v\"\n"
  "[namespace.a.b.properties.q]\n"
  "check = \"r\"\n"
  "[namespace.a.b.properties.q.where]\n"
  "pr = \"r\"\n"
  "[variables]\n"
  "v = { type = \"number\" }\n"
  "t = { type = \"text\" }\n"
  "[monitors.m]\n"
  "inputs = [\"v\"]\n"
  "outputs = [\"p\"]\n"
  "[monitors.v]\n"
  "inputs = [\"t\"]\n";

enum { TOP, P, P_GIVEN, P_CHECK, W, A, B, R, Q, Q_CHECK, PR, V, T, N_ENTITIES };

/* Each entity's kind and name, a `given`'s and a `check`'s its property's,
 * and the entities whose tables hold it, or -1 for the root's or none. */
static const struct {
  ryspec_entity_kind kind;
  const char* name;
  int ns, property;
} expected[N_ENTITIES] = {
  [TOP] = {RYSPEC_ENTITY_RULE, "top", -1, -1},
  [P] = {RYSPEC_ENTITY_PROPERTY, "p", -1, -1},
  [P_GIVEN] = {RYSPEC_ENTITY_GIVEN, "p", -1, P},
  [P_CHECK] = {RYSPEC_ENTITY_CHECK, "p", -1, P},
  [W] = {RYSPEC_ENTITY_PRIVATE_RULE, "w", -1, P},
  [A] = {RYSPEC_ENTITY_NAMESPACE, "a", -1, -1},
  [B] = {RYSPEC_ENTITY_NAMESPACE, "b", A, -1},
  [R] = {RYSPEC_ENTITY_RULE, "r", B, -1},
  [Q] = {RYSPEC_ENTITY_PROPERTY, "q", B, -1},
  [Q_CHECK] = {RYSPEC_ENTITY_CHECK, "q", B, Q},
  [PR] = {RYSPEC_ENTITY_PRIVATE_RULE, "pr", B, Q},
  [V] = {RYSPEC_ENTITY_VARIABLE, "v", -1, -1},
  [T] = {RYSPEC_ENTITY_VARIABLE, "t", -1, -1},
};

/* The entities the visitor met, in order, up to the first MAX_ENTITIES. */
#define MAX_ENTITIES 32

typedef struct seen {
  ryspec_toml_doc_entity at[MAX_ENTITIES];
  size_t n;
} seen;

static int gather(const ryspec_toml_doc_entity* e, void* ctx, ryspec_diag* diag)
{
  (void)diag;
  seen* s = ctx;
  if(s->n < MAX_ENTITIES) {
    s->at[s->n] = *e;
  }
  s->n++;
  return RYSPEC_OK;
}

static ryspec_toml_doc* parse(const char* src, seen* s)
{
  ryspec_diag diag;
  ryspec_toml_doc* doc = ryspec_toml_parse(src, strlen(src), &diag);
  if(!doc) {
    fprintf(stderr, "does not parse: %s\n", diag.message);
    failures++;
    return NULL;
  }
  s->n = 0;
  CHECK(ryspec_toml_doc_each_entity(doc, gather, s, NULL) == RYSPEC_OK);
  return doc;
}

static bool named(const ryspec_toml_doc_entity* e, const char* name)
{
  return e->name && (size_t)e->len == strlen(name) &&
         memcmp(e->name, name, (size_t)e->len) == 0;
}

static void test_entities(const ryspec_toml_doc* doc, const seen* s)
{
  const toml_datum_t* root = ryspec_toml_doc_root(doc);
  CHECK(s->n == N_ENTITIES);
  for(size_t i = 0; i < N_ENTITIES && i < s->n; i++) {
    const ryspec_toml_doc_entity* e = &s->at[i];
    const toml_datum_t* ns = expected[i].kind == RYSPEC_ENTITY_VARIABLE ? NULL
                             : expected[i].ns < 0                       ? root
                                                  : s->at[expected[i].ns].value;
    const toml_datum_t* property =
      expected[i].property < 0 ? NULL : s->at[expected[i].property].value;
    if(
      e->kind != expected[i].kind || !named(e, expected[i].name) ||
      e->ns != ns || e->property != property) {
      fprintf(
        stderr,
        "entity %zu: got kind %d, `%.*s`\n",
        i,
        (int)e->kind,
        e->len,
        e->name ? e->name : "");
      failures++;
    }
  }
  if(s->n != N_ENTITIES) {
    return;
  }

  /* Positions, and a property's parts. */
  int positions = 0;
  for(size_t i = 0; i < s->n; i++) {
    positions += ryspec_toml_doc_is_position(s->at[i].kind);
  }
  CHECK(positions == 7);
  ryspec_toml_doc_entity part;
  CHECK(
    ryspec_toml_doc_property_part(&s->at[P], RYSPEC_ENTITY_GIVEN, &part) &&
    part.value == s->at[P_GIVEN].value);
  CHECK(
    ryspec_toml_doc_property_part(&s->at[Q], RYSPEC_ENTITY_CHECK, &part) &&
    part.value == s->at[Q_CHECK].value && part.ns == s->at[Q].ns &&
    part.property == s->at[Q].value);
  CHECK(!ryspec_toml_doc_property_part(&s->at[Q], RYSPEC_ENTITY_GIVEN, &part));
  CHECK(
    !ryspec_toml_doc_property_part(&s->at[TOP], RYSPEC_ENTITY_CHECK, &part));

  /* Variables, and their types; a monitor is none. */
  CHECK(ryspec_toml_doc_variable(doc, "v", 1) == s->at[V].value);
  CHECK(ryspec_toml_doc_variable(doc, "m", 1) == NULL);
  CHECK(ryspec_toml_doc_variable_type(s->at[V].value) == RYSPEC_TYPE_NUMBER);
  CHECK(ryspec_toml_doc_variable_type(s->at[T].value) == RYSPEC_TYPE_TEXT);
  CHECK(strcmp(ryspec_toml_doc_type_name(RYSPEC_TYPE_TEXT), "text") == 0);

  /* Places: a string's is its first character, inside the quote. */
  CHECK(s->at[TOP].value->lineno == 3 && s->at[TOP].value->colno == 8);
  CHECK(s->at[Q_CHECK].value->lineno == 12);
}

/* A visit stops at the first status but RYSPEC_OK, and returns it. */
static int stop_at_third(
  const ryspec_toml_doc_entity* e, void* ctx, ryspec_diag* diag)
{
  (void)e;
  (void)diag;
  return ++*(int*)ctx == 3 ? RYSPEC_ERROR_SEMANTIC : RYSPEC_OK;
}

static void test_stop(const ryspec_toml_doc* doc)
{
  int count = 0;
  CHECK(
    ryspec_toml_doc_each_entity(doc, stop_at_third, &count, NULL) ==
    RYSPEC_ERROR_SEMANTIC);
  CHECK(count == 3);
}

/* What name resolves to, written at at, NULL being the root. */
static ryspec_toml_doc_target resolve(
  const ryspec_toml_doc* doc,
  const ryspec_toml_doc_entity* at,
  const char* name)
{
  return ryspec_toml_doc_resolve(doc, at, name, strlen(name));
}

static bool resolves(
  const ryspec_toml_doc* doc,
  const ryspec_toml_doc_entity* at,
  const char* name,
  const ryspec_toml_doc_entity* want)
{
  ryspec_toml_doc_target t = resolve(doc, at, name);
  return t.resolution == RYSPEC_RESOLVES_ENTITY &&
         t.entity.kind == want->kind && t.entity.value == want->value &&
         t.entity.ns == want->ns && t.entity.property == want->property;
}

static bool resolves_as(
  const ryspec_toml_doc* doc,
  const ryspec_toml_doc_entity* at,
  const char* name,
  ryspec_resolution want)
{
  ryspec_toml_doc_target t = resolve(doc, at, name);
  return t.resolution == want && t.entity.kind == RYSPEC_ENTITY_NONE;
}

static void test_bare(const ryspec_toml_doc* doc, const seen* s)
{
  const ryspec_toml_doc_entity* e = s->at;
  CHECK(resolves(doc, NULL, "top", &e[TOP]));
  CHECK(resolves(doc, &e[P_CHECK], "p", &e[P]));

  /* A private rule inside its property alone. */
  CHECK(resolves(doc, &e[P_GIVEN], "w", &e[W]));
  CHECK(resolves(doc, &e[W], "w", &e[W]));
  CHECK(resolves(doc, &e[P], "w", &e[W]));
  CHECK(resolves_as(doc, &e[TOP], "w", RYSPEC_RESOLVES_DEDUCED));
  CHECK(resolves_as(doc, NULL, "w", RYSPEC_RESOLVES_DEDUCED));
  CHECK(resolves_as(doc, &e[Q_CHECK], "w", RYSPEC_RESOLVES_DEDUCED));
  CHECK(resolves(doc, &e[Q_CHECK], "pr", &e[PR]));

  /* A namespace's own names inside it alone; neither parent nor child. */
  CHECK(resolves(doc, &e[Q_CHECK], "r", &e[R]));
  CHECK(resolves(doc, &e[B], "r", &e[R]));
  CHECK(resolves_as(doc, NULL, "r", RYSPEC_RESOLVES_DEDUCED));
  CHECK(resolves_as(doc, &e[R], "top", RYSPEC_RESOLVES_DEDUCED));

  /* A top-level namespace at the root alone. */
  CHECK(resolves(doc, NULL, "a", &e[A]));
  CHECK(resolves(doc, &e[P_CHECK], "a", &e[A]));
  CHECK(resolves_as(doc, &e[R], "a", RYSPEC_RESOLVES_DEDUCED));
  CHECK(resolves_as(doc, NULL, "b", RYSPEC_RESOLVES_DEDUCED));

  /* A variable everywhere, seen from a variable as from the root; a
   * monitor nowhere, its name the variable's. */
  CHECK(resolves(doc, NULL, "v", &e[V]));
  CHECK(resolves(doc, &e[PR], "v", &e[V]));
  CHECK(resolves(doc, &e[T], "top", &e[TOP]));
  CHECK(resolves_as(doc, NULL, "m", RYSPEC_RESOLVES_DEDUCED));
  CHECK(resolves_as(doc, NULL, "x", RYSPEC_RESOLVES_DEDUCED));

  /* Types: a variable's as declared, a rule's and a property's bool. */
  ryspec_value_type type;
  CHECK(
    ryspec_toml_doc_target_type(resolve(doc, NULL, "v"), &type) &&
    type == RYSPEC_TYPE_NUMBER);
  CHECK(
    ryspec_toml_doc_target_type(resolve(doc, NULL, "p"), &type) &&
    type == RYSPEC_TYPE_BOOL);
  CHECK(!ryspec_toml_doc_target_type(resolve(doc, NULL, "a"), &type));
  CHECK(!ryspec_toml_doc_target_type(resolve(doc, NULL, "x"), &type));
}

static void test_paths(const ryspec_toml_doc* doc, const seen* s)
{
  const ryspec_toml_doc_entity* e = s->at;
  CHECK(resolves(doc, NULL, "a.b.r", &e[R]));
  CHECK(resolves(doc, &e[P_CHECK], "a.b.q", &e[Q]));
  CHECK(resolves(doc, &e[R], "a.b", &e[B]));
  CHECK(resolves_as(doc, NULL, "a.b.pr", RYSPEC_RESOLVES_NOTHING));
  CHECK(resolves_as(doc, NULL, "a.x.r", RYSPEC_RESOLVES_NOTHING));
  CHECK(resolves_as(doc, NULL, "a.b.zz", RYSPEC_RESOLVES_NOTHING));
  CHECK(resolves_as(doc, NULL, "b.r", RYSPEC_RESOLVES_NOTHING));
  /* A rule or variable midway ends the walk, as does a key no namespace
   * may have. */
  CHECK(resolves_as(doc, NULL, "top.r", RYSPEC_RESOLVES_NOTHING));
  CHECK(resolves_as(doc, NULL, "v.r", RYSPEC_RESOLVES_NOTHING));
  CHECK(resolves_as(doc, NULL, "a.b.rules.r", RYSPEC_RESOLVES_NOTHING));
}

static void test_document(void)
{
  static seen s;
  ryspec_toml_doc* doc = parse(document, &s);
  if(!doc) {
    return;
  }
  test_entities(doc, &s);
  if(s.n == N_ENTITIES) {
    test_stop(doc);
    test_bare(doc, &s);
    test_paths(doc, &s);
  }
  ryspec_toml_doc_free(doc);
}

/* More than one answer is an answer, with one of them. */
static void test_ambiguous(void)
{
  static seen s;
  ryspec_toml_doc* doc = parse(
    "version = \"0\"\n"
    "[rules]\n"
    "x = \"y\"\n"
    "[properties.x]\n"
    "check = \"y\"\n"
    "[variables]\n"
    "y = { type = \"bool\" }\n"
    "[namespace.y.rules]\n"
    "z = \"y\"\n"
    "[namespace.y.properties.z]\n"
    "check = \"y\"\n",
    &s);
  if(!doc) {
    return;
  }
  ryspec_toml_doc_target t = resolve(doc, NULL, "x");
  CHECK(t.resolution == RYSPEC_RESOLVES_AMBIGUOUS);
  CHECK(t.entity.kind == RYSPEC_ENTITY_RULE);
  CHECK(ryspec_toml_doc_target_kind(t) == RYSPEC_ENTITY_NONE);
  t = resolve(doc, NULL, "y");
  CHECK(t.resolution == RYSPEC_RESOLVES_AMBIGUOUS);
  CHECK(t.entity.kind == RYSPEC_ENTITY_NAMESPACE);
  CHECK(resolve(doc, NULL, "y.z").resolution == RYSPEC_RESOLVES_AMBIGUOUS);
  ryspec_toml_doc_free(doc);
}

/* Values of the wrong shape are passed over, or met as they stand. */
static void test_shapes(void)
{
  static seen s;
  ryspec_toml_doc* doc = parse(
    "version = \"0\"\n"
    "namespace = 5\n"
    "[rules]\n"
    "x = 1\n"
    "[variables]\n"
    "bad = 3\n"
    "good = { type = \"what\" }\n"
    "[monitors]\n"
    "m = \"no\"\n"
    "[properties.p]\n"
    "check = 2\n"
    "[extras.namespace.n.rules]\n"
    "e = \"y\"\n",
    &s);
  if(!doc) {
    return;
  }
  /* x, p, its check, good */
  CHECK(s.n == 4);
  if(s.n == 4) {
    CHECK(s.at[0].kind == RYSPEC_ENTITY_RULE);
    CHECK(s.at[1].kind == RYSPEC_ENTITY_PROPERTY);
    CHECK(s.at[2].kind == RYSPEC_ENTITY_CHECK);
    CHECK(s.at[3].kind == RYSPEC_ENTITY_VARIABLE);
    CHECK(ryspec_toml_doc_variable_type(s.at[3].value) == RYSPEC_TYPE_BOOL);
  }
  CHECK(ryspec_toml_doc_variable(doc, "bad", 3) == NULL);
  CHECK(resolves_as(doc, NULL, "bad", RYSPEC_RESOLVES_DEDUCED));
  CHECK(resolves_as(doc, NULL, "e", RYSPEC_RESOLVES_DEDUCED));
  CHECK(resolves_as(doc, NULL, "n.e", RYSPEC_RESOLVES_NOTHING));
  ryspec_toml_doc_free(doc);
}

int main(void)
{
  test_document();
  test_ambiguous();
  test_shapes();
  if(failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  return 0;
}
