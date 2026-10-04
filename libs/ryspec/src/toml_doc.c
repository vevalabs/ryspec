/* Documents: parsing with tomlc17, the translation of expressions, and the
 * index: the entities of a document, and the name index over them. */
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "toml_doc.h"

void ryspec_diagnose(ryspec_diagnostic *diag, ryspec_status status, int line,
                     int column, const char *fmt, ...) {
  if (!diag) {
    return;
  }
  diag->status = status;
  diag->line = line;
  diag->column = column;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(diag->message, sizeof diag->message, fmt, ap);
  va_end(ap);
}

const char *ryspec_version(void) { return RYSPEC_VERSION_STRING; }

/* ---------------------------------------------------------------------------
 * tomlc17. */

toml_datum_t *ryspec_toml_value_lookup(const toml_datum_t *t, const char *key,
                                       size_t key_len) {
  if (!t || t->type != TOML_TABLE) {
    return NULL;
  }
  for (int i = 0; i < t->u.tab.size; i++) {
    if ((size_t)t->u.tab.len[i] == key_len &&
        memcmp(t->u.tab.key[i], key, key_len) == 0) {
      return &t->u.tab.value[i];
    }
  }
  return NULL;
}

toml_option_t ryspec_toml_options(void) {
  /* Off by default in tomlc17; a document must be UTF-8. */
  toml_option_t opt = toml_default_option();
  opt.check_utf8 = true;
  toml_set_option(opt);
  return opt;
}

/* tomlc17 tells its errors apart by message alone, so each kind is known by
 * the words its messages hold, as of the tag CMake pins. Any other message is
 * a syntax error. */
static const struct {
  const char *words;
  ryspec_status status;
} errors[] = {
    {"out of memory", RYSPEC_ERROR_MEMORY},
    {"invalid UTF8 char", RYSPEC_ERROR_TOML_ENCODING},
    {"error converting UCS", RYSPEC_ERROR_TOML_ENCODING},
    {"duplicate key", RYSPEC_ERROR_TOML_REDEFINED},
    {"table defined more than once", RYSPEC_ERROR_TOML_REDEFINED},
    {"table defined before", RYSPEC_ERROR_TOML_REDEFINED},
    {"inline table cannot be extended", RYSPEC_ERROR_TOML_REDEFINED},
    {"cannot extend a static array", RYSPEC_ERROR_TOML_REDEFINED},
    {"cannot extend a previously defined table", RYSPEC_ERROR_TOML_REDEFINED},
    {"cannot locate table", RYSPEC_ERROR_TOML_REDEFINED},
    {"encountered previously declared array", RYSPEC_ERROR_TOML_REDEFINED},
    {"entry must be an array", RYSPEC_ERROR_TOML_REDEFINED},
    {"has no elements", RYSPEC_ERROR_TOML_REDEFINED},
    {"stack overflow", RYSPEC_ERROR_TOML_LIMIT},
    {"too many key parts", RYSPEC_ERROR_TOML_LIMIT},
    {"array too large", RYSPEC_ERROR_TOML_LIMIT},
    {"table too large", RYSPEC_ERROR_TOML_LIMIT},
};

/* The position is a "(line N) " prefix, or for an encoding error an
 * " on line N" suffix; tomlc17 never gives a column. */
void ryspec_toml_error(const char *errmsg, ryspec_diagnostic *diag) {
  int line = 0, prefix = 0;
  if (sscanf(errmsg, "(line %d) %n", &line, &prefix) < 1 || prefix == 0) {
    line = 0;
    prefix = 0;
  }
  const char *message = errmsg + prefix;
  int len = (int)strlen(message);
  const char *suffix = strstr(message, " on line ");
  if (!line && suffix && sscanf(suffix, " on line %d", &line) == 1) {
    len = (int)(suffix - message);
  }
  ryspec_status status = RYSPEC_ERROR_TOML;
  for (size_t i = 0; i < sizeof errors / sizeof errors[0]; i++) {
    if (strstr(message, errors[i].words)) {
      status = errors[i].status;
      break;
    }
  }
  ryspec_diagnose(diag, status, line, 0, "%.*s", len, message);
}

/* ---------------------------------------------------------------------------
 * Documents. */

/* A document holding result, in a block of tomlc17's allocator; NULL, with
 * result freed and diag filled, when that fails. */
static ryspec_toml_doc *block(toml_result_t result, toml_option_t opt,
                              ryspec_diagnostic *diag) {
  ryspec_toml_doc *doc = opt.mem_realloc(NULL, sizeof *doc);
  if (!doc) {
    toml_free(result);
    ryspec_diagnose(diag, RYSPEC_ERROR_MEMORY, 0, 0, "out of memory");
    return NULL;
  }
  *doc = (ryspec_toml_doc){.result = result};
  return doc;
}

/* The document result parses into: its `version` checked, the rest of the
 * schema still to come, its expressions translated, and then indexed. NULL,
 * with diag filled, when it fails. */
static ryspec_toml_doc *finish(toml_result_t result, toml_option_t opt,
                               ryspec_diagnostic *diag) {
  if (!result.ok) {
    ryspec_toml_error(result.errmsg, diag);
    toml_free(result);
    return NULL;
  }
  const toml_datum_t *version =
      ryspec_toml_value_lookup(&result.toptab, "version", 7);
  if (!version) {
    ryspec_diagnose(diag, RYSPEC_ERROR_SCHEMA, 1, 1,
                    "missing required key `version`");
    toml_free(result);
    return NULL;
  }
  if (version->type != TOML_STRING || strcmp(version->u.s, "0") != 0) {
    ryspec_diagnose(diag, RYSPEC_ERROR_SCHEMA, version->lineno,
                    version->colno, "`version` must be the string \"0\"");
    toml_free(result);
    return NULL;
  }
  ryspec_toml_doc *doc = block(result, opt, diag);
  /* Translation may move the block, so the index, which is in it, comes
   * after; until then it is empty, as block() leaves it. */
  if (doc && (ryspec_translate(&doc, opt, diag) != RYSPEC_OK ||
              index_init(doc, diag) != RYSPEC_OK)) {
    ryspec_toml_doc_free(doc);
    return NULL;
  }
  if (doc) {
    ryspec_diagnose(diag, RYSPEC_OK, 0, 0, "");
  }
  return doc;
}

ryspec_toml_doc *ryspec_toml_parse(const char *src, size_t len,
                                   const char *name, ryspec_diagnostic *diag) {
  if (len > (size_t)INT_MAX) {
    ryspec_diagnose(diag, RYSPEC_ERROR_TOML_LIMIT, 0, 0, "input too large");
    return NULL;
  }
  toml_option_t opt = ryspec_toml_options();
  return finish(toml_parse_named(src, (int)len, name), opt, diag);
}

/* The file is opened here, so a missing file is told apart from a parse
 * error; tomlc17 reads it, and says why a read failed in words of its own. */
ryspec_toml_doc *ryspec_toml_parse_file(const char *path,
                                        ryspec_diagnostic *diag) {
  FILE *fp = fopen(path, "rb");
  if (!fp) {
    ryspec_diagnose(diag, RYSPEC_ERROR_IO, 0, 0, "cannot open %s", path);
    return NULL;
  }
  toml_option_t opt = ryspec_toml_options();
  toml_result_t result = toml_parse_file_named(fp, path);
  bool unread = !result.ok && (ferror(fp) ||
                               strcmp(result.errmsg, "file is too big") == 0);
  fclose(fp);
  if (unread) {
    ryspec_diagnose(diag, RYSPEC_ERROR_IO, 0, 0, "cannot read %s: %s", path,
                    result.errmsg);
    toml_free(result);
    return NULL;
  }
  return finish(result, opt, diag);
}

void ryspec_toml_doc_free(ryspec_toml_doc *doc) {
  if (!doc) {
    return;
  }
  index_release(doc);
  toml_free(doc->result);
  /* The allocator a block came from: tomlc17's default, which every parse
   * sets again (ryspec_toml_options()). */
  ryspec_toml_options().mem_free(doc);
}

/* ---------------------------------------------------------------------------
 * The index: building. */

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

/* Add an entity of kind under parent, named by the len bytes at name, for
 * node, returning its id, or RYSPEC_NO_ENTITY when out of memory. */
static ryspec_entity add(ryspec_toml_doc *doc, ryspec_entity_kind kind,
                         ryspec_entity parent, const char *name, int len,
                         const toml_datum_t *node) {
  if (doc->n_entities == doc->cap_entities) {
    toml_option_t opt = ryspec_toml_options();
    size_t want = doc->cap_entities ? doc->cap_entities * 2 : 16;
    index_entity *entities =
        opt.mem_realloc(doc->entities, want * sizeof *entities);
    if (!entities) {
      return RYSPEC_NO_ENTITY;
    }
    doc->entities = entities;
    const toml_datum_t **nodes =
        opt.mem_realloc((void *)doc->nodes, want * sizeof *nodes);
    if (!nodes) {
      return RYSPEC_NO_ENTITY;
    }
    doc->nodes = nodes;
    doc->cap_entities = want;
  }
  ryspec_entity e = doc->n_entities++;
  doc->entities[e] = (index_entity){
      .kind = kind,
      .parent = parent,
      .name = name,
      .len = len,
      .line = node ? node->lineno : 0,
      .column = node ? node->colno : 0,
  };
  doc->nodes[e] = node;
  return e;
}

/* The rules of table t, each of kind, under parent. */
static bool index_rules(ryspec_toml_doc *doc, const toml_datum_t *t,
                        ryspec_entity_kind kind, ryspec_entity parent) {
  if (!is_table(t)) {
    return true;
  }
  for (int i = 0; i < t->u.tab.size; i++) {
    if (add(doc, kind, parent, t->u.tab.key[i], t->u.tab.len[i],
            &t->u.tab.value[i]) == RYSPEC_NO_ENTITY) {
      return false;
    }
  }
  return true;
}

/* A property's `given` or `check`, in *out, which stays RYSPEC_NO_ENTITY
 * where the property has none. */
static bool index_position(ryspec_toml_doc *doc, const toml_datum_t *v,
                           ryspec_entity_kind kind, ryspec_entity property,
                           ryspec_entity *out) {
  *out = RYSPEC_NO_ENTITY;
  if (!v) {
    return true;
  }
  *out = add(doc, kind, property, NULL, 0, v);
  return *out != RYSPEC_NO_ENTITY;
}

/* The rules and properties of the namespace ns. */
static bool index_logic(ryspec_toml_doc *doc, ryspec_entity ns) {
  const toml_datum_t *t = doc->nodes[ns];
  if (!index_rules(doc, get(t, "rules"), RYSPEC_ENTITY_RULE, ns)) {
    return false;
  }
  const toml_datum_t *properties = get(t, "properties");
  if (!is_table(properties)) {
    return true;
  }
  for (int i = 0; i < properties->u.tab.size; i++) {
    const toml_datum_t *p = &properties->u.tab.value[i];
    ryspec_entity at =
        add(doc, RYSPEC_ENTITY_PROPERTY, ns, properties->u.tab.key[i],
            properties->u.tab.len[i], p);
    ryspec_entity given, check;
    if (at == RYSPEC_NO_ENTITY ||
        !index_position(doc, get(p, "given"), RYSPEC_ENTITY_GIVEN, at,
                        &given) ||
        !index_position(doc, get(p, "check"), RYSPEC_ENTITY_CHECK, at,
                        &check) ||
        !index_rules(doc, get(p, "where"), RYSPEC_ENTITY_PRIVATE_RULE, at)) {
      return false;
    }
    doc->entities[at].u.property.given = given;
    doc->entities[at].u.property.check = check;
  }
  return true;
}

/* The named namespaces under t, a table of them, children of parent. */
static bool index_namespaces(ryspec_toml_doc *doc, const toml_datum_t *t,
                             ryspec_entity parent) {
  if (!is_table(t)) {
    return true;
  }
  for (int i = 0; i < t->u.tab.size; i++) {
    const toml_datum_t *v = &t->u.tab.value[i];
    if (!is_table(v) || key_is(t, i, "rules") || key_is(t, i, "properties") ||
        key_is(t, i, "extras")) {
      continue;
    }
    ryspec_entity at = add(doc, RYSPEC_ENTITY_NAMESPACE, parent,
                           t->u.tab.key[i], t->u.tab.len[i], v);
    if (at == RYSPEC_NO_ENTITY || !index_logic(doc, at) ||
        !index_namespaces(doc, v, at)) {
      return false;
    }
  }
  return true;
}

static ryspec_value_type type_of(const toml_datum_t *decl) {
  const toml_datum_t *type = get(decl, "type");
  if (!type || type->type != TOML_STRING) {
    return RYSPEC_TYPE_BOOL;
  }
  if (strcmp(type->u.s, "number") == 0) {
    return RYSPEC_TYPE_NUMBER;
  }
  if (strcmp(type->u.s, "text") == 0) {
    return RYSPEC_TYPE_TEXT;
  }
  if (strcmp(type->u.s, "binary") == 0) {
    return RYSPEC_TYPE_BINARY;
  }
  return RYSPEC_TYPE_BOOL;
}

/* The tables of t, each an entity of kind; a variable gets its type. */
static bool index_tables(ryspec_toml_doc *doc, const toml_datum_t *t,
                         ryspec_entity_kind kind) {
  if (!is_table(t)) {
    return true;
  }
  for (int i = 0; i < t->u.tab.size; i++) {
    const toml_datum_t *v = &t->u.tab.value[i];
    if (!is_table(v)) {
      continue;
    }
    ryspec_entity at =
        add(doc, kind, RYSPEC_NO_ENTITY, t->u.tab.key[i], t->u.tab.len[i], v);
    if (at == RYSPEC_NO_ENTITY) {
      return false;
    }
    if (kind == RYSPEC_ENTITY_VARIABLE) {
      doc->entities[at].u.type = type_of(v);
    }
  }
  return true;
}

/* ---------------------------------------------------------------------------
 * The name index. */

/* The scope e's name is in, or with *in false, none: e is unnamed or a
 * monitor. */
static ryspec_entity scope_of(const ryspec_toml_doc *doc, ryspec_entity e,
                              bool *in) {
  const index_entity *x = &doc->entities[e];
  *in = x->name != NULL;
  switch (x->kind) {
  case RYSPEC_ENTITY_NAMESPACE:
  case RYSPEC_ENTITY_RULE:
  case RYSPEC_ENTITY_PROPERTY:
  case RYSPEC_ENTITY_PRIVATE_RULE:
    return x->parent;
  case RYSPEC_ENTITY_VARIABLE:
    return RYSPEC_NO_ENTITY;
  default:
    *in = false;
    return RYSPEC_NO_ENTITY;
  }
}

/* Order by scope, then name, then entity. */
static int compare(ryspec_entity a_scope, const char *a_name, size_t a_len,
                   ryspec_entity b_scope, const char *b_name, size_t b_len) {
  if (a_scope != b_scope) {
    return a_scope < b_scope ? -1 : 1;
  }
  int c = memcmp(a_name, b_name, a_len < b_len ? a_len : b_len);
  if (c) {
    return c;
  }
  return a_len == b_len ? 0 : a_len < b_len ? -1 : 1;
}

static bool before(const index_name *a, const index_name *b) {
  int c = compare(a->scope, a->name, (size_t)a->len, b->scope, b->name,
                  (size_t)b->len);
  return c ? c < 0 : a->entity < b->entity;
}

/* Restore the heap of the n records at r below i. */
static void sift(index_name *r, size_t i, size_t n) {
  for (size_t child; (child = 2 * i + 1) < n; i = child) {
    if (child + 1 < n && before(&r[child], &r[child + 1])) {
      child++;
    }
    if (!before(&r[i], &r[child])) {
      return;
    }
    index_name t = r[i];
    r[i] = r[child];
    r[child] = t;
  }
}

/* Sort the n records at r in place, by heapsort: qsort() may allocate. */
static void sort_names(index_name *r, size_t n) {
  for (size_t i = n / 2; i-- > 0;) {
    sift(r, i, n);
  }
  for (size_t end = n; end-- > 1;) {
    index_name t = r[0];
    r[0] = r[end];
    r[end] = t;
    sift(r, 0, end);
  }
}

static bool index_names(ryspec_toml_doc *doc) {
  size_t n = 0;
  bool in;
  for (ryspec_entity e = 0; e < doc->n_entities; e++) {
    scope_of(doc, e, &in);
    n += in;
  }
  doc->names =
      ryspec_toml_options().mem_realloc(NULL, (n ? n : 1) * sizeof(index_name));
  if (!doc->names) {
    return false;
  }
  for (ryspec_entity e = 0; e < doc->n_entities; e++) {
    ryspec_entity scope = scope_of(doc, e, &in);
    if (in) {
      doc->names[doc->n_names++] =
          (index_name){scope, e, doc->entities[e].name, doc->entities[e].len};
    }
  }
  sort_names(doc->names, doc->n_names);
  return true;
}

ryspec_status index_init(ryspec_toml_doc *doc, ryspec_diagnostic *diag) {
  const toml_datum_t *root = &doc->result.toptab;
  if (add(doc, RYSPEC_ENTITY_NAMESPACE, RYSPEC_NO_ENTITY, NULL, 0, root) ==
          RYSPEC_NO_ENTITY ||
      !index_logic(doc, 0) ||
      !index_namespaces(doc, get(root, "namespace"), 0) ||
      !index_tables(doc, get(root, "variables"), RYSPEC_ENTITY_VARIABLE) ||
      !index_tables(doc, get(root, "monitors"), RYSPEC_ENTITY_MONITOR) ||
      !index_names(doc)) {
    ryspec_diagnose(diag, RYSPEC_ERROR_MEMORY, 0, 0, "out of memory");
    return RYSPEC_ERROR_MEMORY;
  }
  return RYSPEC_OK;
}

void index_release(ryspec_toml_doc *doc) {
  void (*mem_free)(void *) = ryspec_toml_options().mem_free;
  mem_free(doc->entities);
  mem_free((void *)doc->nodes);
  mem_free(doc->names);
  doc->entities = NULL;
  doc->nodes = NULL;
  doc->names = NULL;
  doc->n_entities = doc->cap_entities = doc->n_names = 0;
}

/* ---------------------------------------------------------------------------
 * The index: entities. */

const index_entity *index_at(const ryspec_toml_doc *doc, ryspec_entity e) {
  return doc && e < doc->n_entities ? &doc->entities[e] : NULL;
}

ryspec_entity_kind index_kind(const ryspec_toml_doc *doc, ryspec_entity e) {
  const index_entity *x = index_at(doc, e);
  return x ? x->kind : RYSPEC_ENTITY_NONE;
}

const toml_datum_t *index_node(const ryspec_toml_doc *doc, ryspec_entity e) {
  return index_at(doc, e) ? doc->nodes[e] : NULL;
}

ryspec_entity index_next(const ryspec_toml_doc *doc, ryspec_entity e,
                         ryspec_entity_kind kind) {
  while (e < doc->n_entities && doc->entities[e].kind != kind) {
    e++;
  }
  return e;
}

bool index_is_position(ryspec_entity_kind kind) {
  return kind == RYSPEC_ENTITY_RULE || kind == RYSPEC_ENTITY_PRIVATE_RULE ||
         kind == RYSPEC_ENTITY_GIVEN || kind == RYSPEC_ENTITY_CHECK;
}

ryspec_entity index_namespace_of(const ryspec_toml_doc *doc, ryspec_entity e) {
  for (const index_entity *x; (x = index_at(doc, e)); e = x->parent) {
    if (x->kind == RYSPEC_ENTITY_NAMESPACE) {
      return e;
    }
  }
  return 0;
}

ryspec_entity index_property_of(const ryspec_toml_doc *doc, ryspec_entity e) {
  for (const index_entity *x; (x = index_at(doc, e)); e = x->parent) {
    if (x->kind == RYSPEC_ENTITY_PROPERTY) {
      return e;
    }
  }
  return RYSPEC_NO_ENTITY;
}

/* ---------------------------------------------------------------------------
 * The index: names. */

const index_name *index_lookup(const ryspec_toml_doc *doc, ryspec_entity scope,
                               const char *name, size_t len, size_t *count) {
  size_t lo = 0, hi = doc->n_names;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    const index_name *m = &doc->names[mid];
    if (compare(m->scope, m->name, (size_t)m->len, scope, name, len) < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  size_t end = lo;
  while (end < doc->n_names &&
         compare(doc->names[end].scope, doc->names[end].name,
                 (size_t)doc->names[end].len, scope, name, len) == 0) {
    end++;
  }
  *count = end - lo;
  return *count ? &doc->names[lo] : NULL;
}

/* What a name answers to, so far: how many entities, and the first. */
typedef struct answers {
  size_t count;
  ryspec_entity first;
} answers;

/* Count the entities of the len bytes at name in scope whose kind is a or
 * b, into *to. */
static void answer(const ryspec_toml_doc *doc, ryspec_entity scope,
                   const char *name, size_t len, ryspec_entity_kind a,
                   ryspec_entity_kind b, answers *to) {
  size_t n;
  const index_name *r = index_lookup(doc, scope, name, len, &n);
  for (size_t i = 0; i < n; i++) {
    ryspec_entity_kind k = doc->entities[r[i].entity].kind;
    if (k == a || k == b) {
      if (!to->count++ || r[i].entity < to->first) {
        to->first = r[i].entity;
      }
    }
  }
}

static index_target target(answers a, ryspec_resolution none) {
  if (a.count == 0) {
    return (index_target){none, RYSPEC_NO_ENTITY};
  }
  return (index_target){a.count == 1 ? RYSPEC_RESOLVES_ENTITY
                                     : RYSPEC_RESOLVES_AMBIGUOUS,
                        a.first};
}

static index_target resolve_path(const ryspec_toml_doc *doc, const char *name,
                                 size_t len) {
  ryspec_entity ns = 0;
  const char *end = name + len;
  for (const char *dot; (dot = memchr(name, '.', (size_t)(end - name)));
       name = dot + 1) {
    answers a = {0, RYSPEC_NO_ENTITY};
    answer(doc, ns, name, (size_t)(dot - name), RYSPEC_ENTITY_NAMESPACE,
           RYSPEC_ENTITY_NAMESPACE, &a);
    if (!a.count) {
      return (index_target){RYSPEC_RESOLVES_NOTHING, RYSPEC_NO_ENTITY};
    }
    ns = a.first;
  }
  answers a = {0, RYSPEC_NO_ENTITY};
  answer(doc, ns, name, (size_t)(end - name), RYSPEC_ENTITY_RULE,
         RYSPEC_ENTITY_PROPERTY, &a);
  if (!a.count) {
    answer(doc, ns, name, (size_t)(end - name), RYSPEC_ENTITY_NAMESPACE,
           RYSPEC_ENTITY_NAMESPACE, &a);
  }
  return target(a, RYSPEC_RESOLVES_NOTHING);
}

index_target index_resolve(const ryspec_toml_doc *doc, ryspec_entity at,
                           const char *name, size_t len) {
  if (memchr(name, '.', len)) {
    return resolve_path(doc, name, len);
  }
  ryspec_entity ns = index_namespace_of(doc, at);
  ryspec_entity property = index_property_of(doc, at);
  answers a = {0, RYSPEC_NO_ENTITY};
  if (property != RYSPEC_NO_ENTITY) {
    answer(doc, property, name, len, RYSPEC_ENTITY_PRIVATE_RULE,
           RYSPEC_ENTITY_PRIVATE_RULE, &a);
  }
  answer(doc, ns, name, len, RYSPEC_ENTITY_RULE, RYSPEC_ENTITY_PROPERTY, &a);
  if (ns == 0) {
    answer(doc, ns, name, len, RYSPEC_ENTITY_NAMESPACE, RYSPEC_ENTITY_NAMESPACE,
           &a);
  }
  answer(doc, RYSPEC_NO_ENTITY, name, len, RYSPEC_ENTITY_VARIABLE,
         RYSPEC_ENTITY_VARIABLE, &a);
  return target(a, RYSPEC_RESOLVES_DEDUCED);
}

ryspec_entity_kind index_target_kind(const ryspec_toml_doc *doc,
                                     index_target t) {
  return t.resolution == RYSPEC_RESOLVES_ENTITY ? index_kind(doc, t.entity)
                                                : RYSPEC_ENTITY_NONE;
}

bool index_target_type(const ryspec_toml_doc *doc, index_target t,
                       ryspec_value_type *out) {
  switch (index_target_kind(doc, t)) {
  case RYSPEC_ENTITY_VARIABLE:
    *out = doc->entities[t.entity].u.type;
    return true;
  case RYSPEC_ENTITY_RULE:
  case RYSPEC_ENTITY_PRIVATE_RULE:
  case RYSPEC_ENTITY_PROPERTY:
    *out = RYSPEC_TYPE_BOOL;
    return true;
  default:
    return false;
  }
}

const char *index_type_name(ryspec_value_type t) {
  static const char *names[] = {"bool", "number", "text", "binary"};
  return names[t];
}

ryspec_entity index_variable_find(const ryspec_toml_doc *doc, const char *name,
                                  size_t len) {
  size_t n;
  const index_name *r = index_lookup(doc, RYSPEC_NO_ENTITY, name, len, &n);
  return n ? r[0].entity : RYSPEC_NO_ENTITY;
}

/* ---------------------------------------------------------------------------
 * The index: monitors. */

ryspec_entity index_monitor_find(const ryspec_toml_doc *doc, const char *name,
                                 size_t len) {
  for (ryspec_entity e = 0; e < doc->n_entities; e++) {
    const index_entity *x = &doc->entities[e];
    if (x->kind == RYSPEC_ENTITY_MONITOR && (size_t)x->len == len &&
        memcmp(x->name, name, len) == 0) {
      return e;
    }
  }
  return RYSPEC_NO_ENTITY;
}

const toml_datum_t *index_monitor_list(const ryspec_toml_doc *doc,
                                       ryspec_entity m,
                                       ryspec_monitor_list list) {
  static const char *const keys[] = {"inputs", "parameters", "outputs"};
  if (index_kind(doc, m) != RYSPEC_ENTITY_MONITOR) {
    return NULL;
  }
  const toml_datum_t *v = get(doc->nodes[m], keys[list]);
  return v && v->type == TOML_ARRAY ? v : NULL;
}

bool index_array_holds(const toml_datum_t *a, const char *name, size_t len) {
  for (int i = 0; a && i < a->u.arr.size; i++) {
    const toml_datum_t *e = &a->u.arr.elem[i];
    if (e->type == TOML_STRING && (size_t)e->u.str.len == len &&
        memcmp(e->u.str.ptr, name, len) == 0) {
      return true;
    }
  }
  return false;
}

bool index_monitor_lists(const ryspec_toml_doc *doc, ryspec_entity m,
                         const char *name, size_t len) {
  return index_array_holds(index_monitor_list(doc, m, RYSPEC_MONITOR_INPUTS),
                           name, len) ||
         index_array_holds(
             index_monitor_list(doc, m, RYSPEC_MONITOR_PARAMETERS), name,
             len) ||
         index_array_holds(index_monitor_list(doc, m, RYSPEC_MONITOR_OUTPUTS),
                           name, len);
}

/* ---------------------------------------------------------------------------
 * The public interface: ryspec.h's, total over any id. */

size_t ryspec_entity_count(const ryspec_toml_doc *doc) {
  return doc ? doc->n_entities : 0;
}

ryspec_entity_kind ryspec_entity_kind_of(const ryspec_toml_doc *doc,
                                         ryspec_entity entity) {
  return index_kind(doc, entity);
}

ryspec_entity ryspec_entity_parent(const ryspec_toml_doc *doc,
                                   ryspec_entity entity) {
  const index_entity *x = index_at(doc, entity);
  return x ? x->parent : RYSPEC_NO_ENTITY;
}

const char *ryspec_entity_name(const ryspec_toml_doc *doc, ryspec_entity entity,
                               size_t *len) {
  const index_entity *x = index_at(doc, entity);
  const char *name = x ? x->name : NULL;
  if (len) {
    *len = name ? (size_t)x->len : 0;
  }
  return name;
}

/* Write e's path into buf from at on, returning where it ends: its named
 * ancestors' first, each followed by a dot. */
static size_t write_path(const ryspec_toml_doc *doc, ryspec_entity e, char *buf,
                         size_t size, size_t at) {
  const index_entity *x = index_at(doc, e);
  const index_entity *up = index_at(doc, x->parent);
  if (up && up->name) {
    at = write_path(doc, x->parent, buf, size, at);
    if (at < size) {
      buf[at] = '.';
    }
    at++;
  }
  for (int i = 0; i < x->len; i++, at++) {
    if (at < size) {
      buf[at] = x->name[i];
    }
  }
  return at;
}

size_t ryspec_entity_path(const ryspec_toml_doc *doc, ryspec_entity entity,
                          char *buf, size_t size) {
  const index_entity *x = index_at(doc, entity);
  size_t len = x && x->name ? write_path(doc, entity, buf, size, 0) : 0;
  if (size) {
    buf[len < size ? len : size - 1] = '\0';
  }
  return len;
}

bool ryspec_entity_place(const ryspec_toml_doc *doc, ryspec_entity entity,
                         int *line, int *column) {
  const index_entity *x = index_at(doc, entity);
  bool placed = x && x->line > 0;
  if (line) {
    *line = placed ? x->line : 0;
  }
  if (column) {
    *column = placed ? x->column : 0;
  }
  return placed;
}
