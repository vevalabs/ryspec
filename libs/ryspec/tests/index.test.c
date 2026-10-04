/* Tests of the index: its entities in document order, their parents, the
 * name index and its scopes, and the resolution of names over them; then
 * the public interface, ryspec.h's, alone, invalid ids among what it is
 * given. They take no argument. */
#include "ryspec/ryspec.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "toml_doc.h"

static int failures;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
      failures++;                                                              \
    }                                                                          \
  } while (0)

/* A document of every kind of entity, a namespace two deep among them, and
 * a monitor sharing its name with a variable. Its entities by id: */
static const char document[] = "version = \"0\"\n"
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

enum {
  ROOT,
  TOP,
  P,
  P_GIVEN,
  P_CHECK,
  W,
  A,
  B,
  R,
  Q,
  Q_CHECK,
  PR,
  V,
  T,
  M,
  MV,
  N_ENTITIES
};

static const struct {
  ryspec_entity_kind kind;
  ryspec_entity parent;
  const char *name;
} expected[N_ENTITIES] = {
    [ROOT] = {RYSPEC_ENTITY_NAMESPACE, RYSPEC_NO_ENTITY, NULL},
    [TOP] = {RYSPEC_ENTITY_RULE, ROOT, "top"},
    [P] = {RYSPEC_ENTITY_PROPERTY, ROOT, "p"},
    [P_GIVEN] = {RYSPEC_ENTITY_GIVEN, P, NULL},
    [P_CHECK] = {RYSPEC_ENTITY_CHECK, P, NULL},
    [W] = {RYSPEC_ENTITY_PRIVATE_RULE, P, "w"},
    [A] = {RYSPEC_ENTITY_NAMESPACE, ROOT, "a"},
    [B] = {RYSPEC_ENTITY_NAMESPACE, A, "b"},
    [R] = {RYSPEC_ENTITY_RULE, B, "r"},
    [Q] = {RYSPEC_ENTITY_PROPERTY, B, "q"},
    [Q_CHECK] = {RYSPEC_ENTITY_CHECK, Q, NULL},
    [PR] = {RYSPEC_ENTITY_PRIVATE_RULE, Q, "pr"},
    [V] = {RYSPEC_ENTITY_VARIABLE, RYSPEC_NO_ENTITY, "v"},
    [T] = {RYSPEC_ENTITY_VARIABLE, RYSPEC_NO_ENTITY, "t"},
    [M] = {RYSPEC_ENTITY_MONITOR, RYSPEC_NO_ENTITY, "m"},
    [MV] = {RYSPEC_ENTITY_MONITOR, RYSPEC_NO_ENTITY, "v"},
};

static ryspec_toml_doc *parse(const char *src) {
  ryspec_diagnostic diag;
  ryspec_toml_doc *doc = ryspec_toml_parse(src, strlen(src), NULL, &diag);
  if (!doc) {
    fprintf(stderr, "does not parse: %s\n", diag.message);
    failures++;
  }
  return doc;
}

static bool named(const index_entity *x, const char *name) {
  if (!name || !x->name) {
    return !name && !x->name;
  }
  return (size_t)x->len == strlen(name) &&
         memcmp(x->name, name, (size_t)x->len) == 0;
}

/* What name resolves to, written at at. */
static index_target resolve(const ryspec_toml_doc *doc, ryspec_entity at,
                            const char *name) {
  return index_resolve(doc, at, name, strlen(name));
}

static bool resolves(const ryspec_toml_doc *doc, ryspec_entity at,
                     const char *name, ryspec_entity want) {
  index_target t = resolve(doc, at, name);
  return t.resolution == RYSPEC_RESOLVES_ENTITY && t.entity == want;
}

static bool resolves_as(const ryspec_toml_doc *doc, ryspec_entity at,
                        const char *name, ryspec_resolution want) {
  index_target t = resolve(doc, at, name);
  return t.resolution == want && t.entity == RYSPEC_NO_ENTITY;
}

static void test_entities(const ryspec_toml_doc *doc) {
  CHECK(doc->n_entities == N_ENTITIES);
  for (ryspec_entity e = 0; e < N_ENTITIES && e < doc->n_entities; e++) {
    const index_entity *x = index_at(doc, e);
    if (x->kind != expected[e].kind || x->parent != expected[e].parent ||
        !named(x, expected[e].name)) {
      fprintf(stderr, "entity %zu: got kind %d, parent %zu, `%.*s`\n", e,
              (int)x->kind, x->parent, x->len, x->name ? x->name : "");
      failures++;
    }
  }
  CHECK(index_at(doc, N_ENTITIES) == NULL);
  CHECK(index_at(doc, RYSPEC_NO_ENTITY) == NULL);
  CHECK(index_kind(doc, RYSPEC_NO_ENTITY) == RYSPEC_ENTITY_NONE);
  CHECK(index_node(doc, RYSPEC_NO_ENTITY) == NULL);

  /* A property's positions, and a variable's type. */
  CHECK(doc->entities[P].u.property.given == P_GIVEN);
  CHECK(doc->entities[P].u.property.check == P_CHECK);
  CHECK(doc->entities[Q].u.property.given == RYSPEC_NO_ENTITY);
  CHECK(doc->entities[Q].u.property.check == Q_CHECK);
  CHECK(doc->entities[V].u.type == RYSPEC_TYPE_NUMBER);
  CHECK(doc->entities[T].u.type == RYSPEC_TYPE_TEXT);

  /* Places: a rule's is its value's, the root's the document's start. */
  CHECK(doc->entities[TOP].line == 3);
  CHECK(doc->entities[Q_CHECK].line == 12);
  CHECK(doc->entities[ROOT].line == 1 && doc->entities[ROOT].column == 1);

  /* Positions, and the iterator over a kind. */
  int positions = 0;
  for (ryspec_entity e = 0; e < doc->n_entities; e++) {
    positions += index_is_position(doc->entities[e].kind);
  }
  CHECK(positions == 7);
  ryspec_entity e = index_next(doc, 0, RYSPEC_ENTITY_MONITOR);
  CHECK(e == M);
  e = index_next(doc, e + 1, RYSPEC_ENTITY_MONITOR);
  CHECK(e == MV);
  CHECK(index_next(doc, e + 1, RYSPEC_ENTITY_MONITOR) == doc->n_entities);
}

static void test_scopes(const ryspec_toml_doc *doc) {
  CHECK(index_namespace_of(doc, RYSPEC_NO_ENTITY) == ROOT);
  CHECK(index_namespace_of(doc, P_GIVEN) == ROOT);
  CHECK(index_namespace_of(doc, PR) == B);
  CHECK(index_namespace_of(doc, B) == B);
  CHECK(index_namespace_of(doc, V) == ROOT);
  CHECK(index_property_of(doc, PR) == Q);
  CHECK(index_property_of(doc, Q) == Q);
  CHECK(index_property_of(doc, R) == RYSPEC_NO_ENTITY);
  CHECK(index_property_of(doc, RYSPEC_NO_ENTITY) == RYSPEC_NO_ENTITY);

  /* Every named entity but a monitor is in the name index, once. */
  CHECK(doc->n_names == 10);
  size_t n;
  const index_name *r = index_lookup(doc, ROOT, "top", 3, &n);
  CHECK(n == 1 && r && r->entity == TOP);
  r = index_lookup(doc, P, "w", 1, &n);
  CHECK(n == 1 && r && r->entity == W);
  r = index_lookup(doc, A, "b", 1, &n);
  CHECK(n == 1 && r && r->entity == B);
  r = index_lookup(doc, RYSPEC_NO_ENTITY, "v", 1, &n);
  CHECK(n == 1 && r && r->entity == V);
  CHECK(index_lookup(doc, ROOT, "w", 1, &n) == NULL && n == 0);
  CHECK(index_lookup(doc, ROOT, "m", 1, &n) == NULL && n == 0);
  CHECK(index_lookup(doc, RYSPEC_NO_ENTITY, "m", 1, &n) == NULL && n == 0);
  for (size_t i = 1; i < doc->n_names; i++) {
    CHECK(doc->names[i - 1].scope <= doc->names[i].scope);
  }
}

static void test_bare(const ryspec_toml_doc *doc) {
  CHECK(resolves(doc, RYSPEC_NO_ENTITY, "top", TOP));
  CHECK(resolves(doc, P_CHECK, "p", P));

  /* A private rule inside its property alone. */
  CHECK(resolves(doc, P_GIVEN, "w", W));
  CHECK(resolves(doc, W, "w", W));
  CHECK(resolves_as(doc, TOP, "w", RYSPEC_RESOLVES_DEDUCED));
  CHECK(resolves_as(doc, RYSPEC_NO_ENTITY, "w", RYSPEC_RESOLVES_DEDUCED));
  CHECK(resolves_as(doc, Q_CHECK, "w", RYSPEC_RESOLVES_DEDUCED));
  CHECK(resolves(doc, Q_CHECK, "pr", PR));

  /* A namespace's own names inside it alone; neither parent nor child. */
  CHECK(resolves(doc, Q_CHECK, "r", R));
  CHECK(resolves_as(doc, RYSPEC_NO_ENTITY, "r", RYSPEC_RESOLVES_DEDUCED));
  CHECK(resolves_as(doc, R, "top", RYSPEC_RESOLVES_DEDUCED));

  /* A top-level namespace at the root alone. */
  CHECK(resolves(doc, RYSPEC_NO_ENTITY, "a", A));
  CHECK(resolves(doc, P_CHECK, "a", A));
  CHECK(resolves_as(doc, R, "a", RYSPEC_RESOLVES_DEDUCED));
  CHECK(resolves_as(doc, RYSPEC_NO_ENTITY, "b", RYSPEC_RESOLVES_DEDUCED));

  /* A variable everywhere; a monitor nowhere, its name the variable's. */
  CHECK(resolves(doc, RYSPEC_NO_ENTITY, "v", V));
  CHECK(resolves(doc, PR, "v", V));
  CHECK(resolves_as(doc, RYSPEC_NO_ENTITY, "m", RYSPEC_RESOLVES_DEDUCED));
  CHECK(resolves_as(doc, RYSPEC_NO_ENTITY, "x", RYSPEC_RESOLVES_DEDUCED));
}

static void test_paths(const ryspec_toml_doc *doc) {
  CHECK(resolves(doc, RYSPEC_NO_ENTITY, "a.b.r", R));
  CHECK(resolves(doc, P_CHECK, "a.b.q", Q));
  CHECK(resolves(doc, R, "a.b", B));
  CHECK(resolves_as(doc, RYSPEC_NO_ENTITY, "a.b.pr", RYSPEC_RESOLVES_NOTHING));
  CHECK(resolves_as(doc, RYSPEC_NO_ENTITY, "a.x.r", RYSPEC_RESOLVES_NOTHING));
  CHECK(resolves_as(doc, RYSPEC_NO_ENTITY, "a.b.zz", RYSPEC_RESOLVES_NOTHING));
  CHECK(resolves_as(doc, RYSPEC_NO_ENTITY, "b.r", RYSPEC_RESOLVES_NOTHING));
  /* A rule or variable midway ends the walk. */
  CHECK(resolves_as(doc, RYSPEC_NO_ENTITY, "top.r", RYSPEC_RESOLVES_NOTHING));
  CHECK(resolves_as(doc, RYSPEC_NO_ENTITY, "v.r", RYSPEC_RESOLVES_NOTHING));
}

static void test_monitors(const ryspec_toml_doc *doc) {
  CHECK(index_monitor_find(doc, "m", 1) == M);
  CHECK(index_monitor_find(doc, "v", 1) == MV);
  CHECK(index_monitor_find(doc, "p", 1) == RYSPEC_NO_ENTITY);
  CHECK(index_monitor_list(doc, M, RYSPEC_MONITOR_OUTPUTS) != NULL);
  CHECK(index_monitor_list(doc, MV, RYSPEC_MONITOR_OUTPUTS) == NULL);
  CHECK(index_monitor_list(doc, V, RYSPEC_MONITOR_INPUTS) == NULL);
  CHECK(index_monitor_lists(doc, M, "p", 1));
  CHECK(!index_monitor_lists(doc, M, "t", 1));
  CHECK(index_variable_find(doc, "v", 1) == V);
  CHECK(index_variable_find(doc, "m", 1) == RYSPEC_NO_ENTITY);
}

static void test_document(void) {
  ryspec_toml_doc *doc = parse(document);
  if (!doc) {
    return;
  }
  test_entities(doc);
  test_scopes(doc);
  test_bare(doc);
  test_paths(doc);
  test_monitors(doc);
  ryspec_toml_doc_free(doc);
}

/* More than one answer is an answer: the first by id with it. */
static void test_ambiguous(void) {
  ryspec_toml_doc *doc = parse("version = \"0\"\n"
                               "[rules]\n"
                               "x = \"y\"\n"
                               "[properties.x]\n"
                               "check = \"y\"\n"
                               "[variables]\n"
                               "y = { type = \"bool\" }\n"
                               "[namespace.y.rules]\n"
                               "z = \"y\"\n"
                               "[namespace.y.properties.z]\n"
                               "check = \"y\"\n");
  if (!doc) {
    return;
  }
  index_target t = resolve(doc, RYSPEC_NO_ENTITY, "x");
  CHECK(t.resolution == RYSPEC_RESOLVES_AMBIGUOUS);
  CHECK(index_kind(doc, t.entity) == RYSPEC_ENTITY_RULE);
  CHECK(index_target_kind(doc, t) == RYSPEC_ENTITY_NONE);
  t = resolve(doc, RYSPEC_NO_ENTITY, "y");
  CHECK(t.resolution == RYSPEC_RESOLVES_AMBIGUOUS);
  CHECK(index_kind(doc, t.entity) == RYSPEC_ENTITY_NAMESPACE);
  CHECK(resolve(doc, RYSPEC_NO_ENTITY, "y.z").resolution ==
        RYSPEC_RESOLVES_AMBIGUOUS);
  ryspec_toml_doc_free(doc);
}

/* Values of the wrong shape are passed over, or indexed as they stand. */
static void test_shapes(void) {
  ryspec_toml_doc *doc = parse("version = \"0\"\n"
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
                               "e = \"y\"\n");
  if (!doc) {
    return;
  }
  /* root, x, p, its check, good */
  CHECK(doc->n_entities == 5);
  CHECK(index_kind(doc, 1) == RYSPEC_ENTITY_RULE);
  CHECK(index_kind(doc, 3) == RYSPEC_ENTITY_CHECK);
  CHECK(index_kind(doc, 4) == RYSPEC_ENTITY_VARIABLE);
  CHECK(doc->entities[4].u.type == RYSPEC_TYPE_BOOL);
  ryspec_toml_doc_free(doc);
}

/* The public interface, over the document above. */

static bool path_is(const ryspec_toml_doc *doc, ryspec_entity e,
                    const char *want) {
  char buf[64];
  size_t len = ryspec_entity_path(doc, e, buf, sizeof buf);
  return len == strlen(want) && strcmp(buf, want) == 0;
}

static void test_public_entities(const ryspec_toml_doc *doc) {
  CHECK(ryspec_entity_count(doc) == N_ENTITIES);
  for (ryspec_entity e = 0; e < N_ENTITIES; e++) {
    size_t len;
    const char *name = ryspec_entity_name(doc, e, &len);
    CHECK(ryspec_entity_kind_of(doc, e) == expected[e].kind);
    CHECK(ryspec_entity_parent(doc, e) == expected[e].parent);
    CHECK(expected[e].name ? name && len == strlen(expected[e].name) &&
                                 memcmp(name, expected[e].name, len) == 0
                           : !name && len == 0);
  }
  /* A string's place is its first character, inside the quote. */
  int line, column;
  CHECK(ryspec_entity_place(doc, TOP, &line, &column) && line == 3 &&
        column == 8);
  CHECK(ryspec_entity_place(doc, Q, NULL, NULL));
}

static void test_public_paths(const ryspec_toml_doc *doc) {
  CHECK(path_is(doc, TOP, "top"));
  CHECK(path_is(doc, P, "p"));
  CHECK(path_is(doc, W, "p.w"));
  CHECK(path_is(doc, A, "a"));
  CHECK(path_is(doc, B, "a.b"));
  CHECK(path_is(doc, R, "a.b.r"));
  CHECK(path_is(doc, Q, "a.b.q"));
  CHECK(path_is(doc, PR, "a.b.q.pr"));
  CHECK(path_is(doc, V, "v"));
  CHECK(path_is(doc, MV, "v"));
  CHECK(path_is(doc, ROOT, ""));
  CHECK(path_is(doc, P_GIVEN, ""));
  CHECK(path_is(doc, Q_CHECK, ""));

  /* As snprintf(): the full length, the text cut to fit. */
  char buf[5] = "xxxx";
  CHECK(ryspec_entity_path(doc, PR, NULL, 0) == 8);
  CHECK(ryspec_entity_path(doc, PR, buf, 0) == 8 && buf[0] == 'x');
  CHECK(ryspec_entity_path(doc, PR, buf, 1) == 8 && buf[0] == '\0');
  CHECK(ryspec_entity_path(doc, PR, buf, sizeof buf) == 8 &&
        strcmp(buf, "a.b.") == 0);
}

/* An id past the last, or none, or no document, reads as no entity. */
static void test_public_invalid(const ryspec_toml_doc *doc) {
  const ryspec_toml_doc *docs[] = {doc, NULL};
  const ryspec_entity ids[] = {RYSPEC_NO_ENTITY, N_ENTITIES, SIZE_MAX - 1};
  CHECK(ryspec_entity_count(NULL) == 0);
  for (int i = 0; i < 2; i++) {
    for (int k = 0; k < 3; k++) {
      const ryspec_toml_doc *x = docs[i];
      ryspec_entity e = ids[k];
      size_t len = 1;
      int line = 1, column = 1;
      char buf[8] = "xxxxxxx";
      CHECK(ryspec_entity_kind_of(x, e) == RYSPEC_ENTITY_NONE);
      CHECK(ryspec_entity_parent(x, e) == RYSPEC_NO_ENTITY);
      CHECK(ryspec_entity_name(x, e, &len) == NULL && len == 0);
      CHECK(ryspec_entity_name(x, e, NULL) == NULL);
      CHECK(ryspec_entity_path(x, e, buf, sizeof buf) == 0 && buf[0] == '\0');
      CHECK(ryspec_entity_path(x, e, NULL, 0) == 0);
      CHECK(!ryspec_entity_place(x, e, &line, &column) && line == 0 &&
            column == 0);
      CHECK(!ryspec_entity_place(x, e, NULL, NULL));
    }
  }
  /* Valid ids, NULL out-pointers. */
  CHECK(ryspec_entity_name(doc, TOP, NULL) != NULL);
  CHECK(ryspec_entity_name(doc, ROOT, NULL) == NULL);
}

static void test_public(void) {
  ryspec_toml_doc *doc = parse(document);
  if (doc) {
    test_public_entities(doc);
    test_public_paths(doc);
    test_public_invalid(doc);
  }
  ryspec_toml_doc_free(doc);
}

int main(void) {
  test_document();
  test_ambiguous();
  test_shapes();
  test_public();
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  return 0;
}
