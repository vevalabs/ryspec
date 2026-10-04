/* Documents: parsing with tomlc17 and the translation of expressions; and
 * reading them: the visitor of their entities, and the resolver of names. */
#include "toml_doc.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void ryspec_diagnose(
  ryspec_diag* diag, int status, int line, int column, const char* fmt, ...)
{
  if(!diag) {
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

int ryspec_version(void)
{
  return RYSPEC_VERSION_NUMBER;
}

/* ---------------------------------------------------------------------------
 * tomlc17. */

toml_datum_t* ryspec_toml_value_lookup(
  const toml_datum_t* t, const char* key, size_t key_len)
{
  if(!t || t->type != TOML_TABLE) {
    return NULL;
  }
  for(int i = 0; i < t->u.tab.size; i++) {
    if(
      (size_t)t->u.tab.len[i] == key_len &&
      memcmp(t->u.tab.key[i], key, key_len) == 0) {
      return &t->u.tab.value[i];
    }
  }
  return NULL;
}

toml_option_t ryspec_toml_options(void)
{
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
  const char* words;
  int status;
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
void ryspec_toml_error(const char* errmsg, ryspec_diag* diag)
{
  int line = 0, prefix = 0;
  if(sscanf(errmsg, "(line %d) %n", &line, &prefix) < 1 || prefix == 0) {
    line = 0;
    prefix = 0;
  }
  const char* message = errmsg + prefix;
  int len = (int)strlen(message);
  const char* suffix = strstr(message, " on line ");
  if(!line && suffix && sscanf(suffix, " on line %d", &line) == 1) {
    len = (int)(suffix - message);
  }
  int status = RYSPEC_ERROR_TOML;
  for(size_t i = 0; i < sizeof errors / sizeof errors[0]; i++) {
    if(strstr(message, errors[i].words)) {
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
static ryspec_toml_doc* block(
  toml_result_t result, toml_option_t opt, ryspec_diag* diag)
{
  ryspec_toml_doc* doc = opt.mem_realloc(NULL, sizeof *doc);
  if(!doc) {
    toml_free(result);
    ryspec_diagnose(diag, RYSPEC_ERROR_MEMORY, 0, 0, "out of memory");
    return NULL;
  }
  *doc = (ryspec_toml_doc){.result = result};
  return doc;
}

/* The document result parses into: its `version` checked, the rest of the
 * schema still to come, and its expressions translated. NULL, with diag
 * filled, when it fails. */
static ryspec_toml_doc* finish(
  toml_result_t result, toml_option_t opt, ryspec_diag* diag)
{
  if(!result.ok) {
    ryspec_toml_error(result.errmsg, diag);
    toml_free(result);
    return NULL;
  }
  const toml_datum_t* version =
    ryspec_toml_value_lookup(&result.toptab, "version", 7);
  if(!version) {
    ryspec_diagnose(
      diag, RYSPEC_ERROR_SCHEMA, 1, 1, "missing required key `version`");
    toml_free(result);
    return NULL;
  }
  if(version->type != TOML_STRING || strcmp(version->u.s, "0") != 0) {
    ryspec_diagnose(
      diag,
      RYSPEC_ERROR_SCHEMA,
      version->lineno,
      version->colno,
      "`version` must be the string \"0\"");
    toml_free(result);
    return NULL;
  }
  ryspec_toml_doc* doc = block(result, opt, diag);
  if(doc && ryspec_translate(&doc, opt, diag) != RYSPEC_OK) {
    ryspec_toml_doc_free(doc);
    return NULL;
  }
  if(doc) {
    ryspec_diagnose(diag, RYSPEC_OK, 0, 0, "");
  }
  return doc;
}

ryspec_toml_doc* ryspec_toml_parse(
  const char* src, size_t len, ryspec_diag* diag)
{
  if(len > (size_t)INT_MAX) {
    ryspec_diagnose(diag, RYSPEC_ERROR_TOML_LIMIT, 0, 0, "input too large");
    return NULL;
  }
  toml_option_t opt = ryspec_toml_options();
  return finish(toml_parse(src, (int)len), opt, diag);
}

/* The file is opened here, so a missing file is told apart from a parse
 * error; tomlc17 reads it, and says why a read failed in words of its own. */
ryspec_toml_doc* ryspec_toml_parse_file(const char* path, ryspec_diag* diag)
{
  FILE* fp = fopen(path, "rb");
  if(!fp) {
    ryspec_diagnose(diag, RYSPEC_ERROR_IO, 0, 0, "cannot open %s", path);
    return NULL;
  }
  toml_option_t opt = ryspec_toml_options();
  toml_result_t result = toml_parse_file(fp);
  bool unread =
    !result.ok && (ferror(fp) || strcmp(result.errmsg, "file is too big") == 0);
  fclose(fp);
  if(unread) {
    ryspec_diagnose(
      diag, RYSPEC_ERROR_IO, 0, 0, "cannot read %s: %s", path, result.errmsg);
    toml_free(result);
    return NULL;
  }
  return finish(result, opt, diag);
}

void ryspec_toml_doc_free(ryspec_toml_doc* doc)
{
  if(!doc) {
    return;
  }
  toml_free(doc->result);
  /* The allocator a block came from: tomlc17's default, which every parse
   * sets again (ryspec_toml_options()). */
  ryspec_toml_options().mem_free(doc);
}

/* ---------------------------------------------------------------------------
 * Entities. */

static const toml_datum_t* get(const toml_datum_t* t, const char* key)
{
  return ryspec_toml_value_lookup(t, key, strlen(key));
}

static bool is_table(const toml_datum_t* d)
{
  return d && d->type == TOML_TABLE;
}

static bool key_is(const toml_datum_t* t, int i, const char* key)
{
  return (size_t)t->u.tab.len[i] == strlen(key) &&
         memcmp(t->u.tab.key[i], key, (size_t)t->u.tab.len[i]) == 0;
}

/* Whether the i-th entry of t, a table of namespaces, is one: a table, and
 * not under a key a namespace may not have. */
static bool is_namespace(const toml_datum_t* t, int i)
{
  return is_table(&t->u.tab.value[i]) && !key_is(t, i, "rules") &&
         !key_is(t, i, "properties") && !key_is(t, i, "extras");
}

const toml_datum_t* ryspec_toml_doc_root(const ryspec_toml_doc* doc)
{
  return &doc->result.toptab;
}

/* The table holding the named namespaces of the namespace ns: the root's
 * `namespace`, or a named namespace's own table. */
static const toml_datum_t* namespaces_of(
  const ryspec_toml_doc* doc, const toml_datum_t* ns)
{
  return ns == ryspec_toml_doc_root(doc) ? get(ns, "namespace") : ns;
}

/* The entity of kind under the i-th key of t, in ns and property. */
static ryspec_toml_doc_entity entry(
  ryspec_entity_kind kind,
  const toml_datum_t* t,
  int i,
  const toml_datum_t* ns,
  const toml_datum_t* property)
{
  return (ryspec_toml_doc_entity){
    kind, t->u.tab.key[i], t->u.tab.len[i], &t->u.tab.value[i], ns, property};
}

bool ryspec_toml_doc_property_part(
  const ryspec_toml_doc_entity* p,
  ryspec_entity_kind part,
  ryspec_toml_doc_entity* out)
{
  const toml_datum_t* v = p->kind != RYSPEC_ENTITY_PROPERTY ? NULL
                          : part == RYSPEC_ENTITY_GIVEN ? get(p->value, "given")
                          : part == RYSPEC_ENTITY_CHECK ? get(p->value, "check")
                                                        : NULL;
  if(!v) {
    return false;
  }
  *out = (ryspec_toml_doc_entity){part, p->name, p->len, v, p->ns, p->value};
  return true;
}

typedef struct visit {
  const ryspec_toml_doc* doc;
  ryspec_toml_doc_entity_fn fn;
  void* ctx;
  ryspec_diag* diag;
} visit;

/* Each entry of t as an entity of kind, in ns and property; only the
 * tables with tables_only. */
static int visit_table(
  const visit* v,
  const toml_datum_t* t,
  ryspec_entity_kind kind,
  bool tables_only,
  const toml_datum_t* ns,
  const toml_datum_t* property)
{
  int s = RYSPEC_OK;
  for(int i = 0; s == RYSPEC_OK && is_table(t) && i < t->u.tab.size; i++) {
    if(!tables_only || is_table(&t->u.tab.value[i])) {
      ryspec_toml_doc_entity e = entry(kind, t, i, ns, property);
      s = v->fn(&e, v->ctx, v->diag);
    }
  }
  return s;
}

/* A property's parts: its `given`, its `check` and its private rules. */
static int visit_parts(const visit* v, const ryspec_toml_doc_entity* p)
{
  static const ryspec_entity_kind parts[] = {
    RYSPEC_ENTITY_GIVEN, RYSPEC_ENTITY_CHECK};
  for(int k = 0; k < 2; k++) {
    ryspec_toml_doc_entity e;
    if(ryspec_toml_doc_property_part(p, parts[k], &e)) {
      int s = v->fn(&e, v->ctx, v->diag);
      if(s != RYSPEC_OK) {
        return s;
      }
    }
  }
  return visit_table(
    v,
    get(p->value, "where"),
    RYSPEC_ENTITY_PRIVATE_RULE,
    false,
    p->ns,
    p->value);
}

/* The namespace of table ns: its rules, its properties and their parts,
 * then its named namespaces, each so in turn. */
static int visit_namespace(const visit* v, const toml_datum_t* ns)
{
  int s = visit_table(v, get(ns, "rules"), RYSPEC_ENTITY_RULE, false, ns, NULL);
  const toml_datum_t* properties = get(ns, "properties");
  for(int i = 0;
      s == RYSPEC_OK && is_table(properties) && i < properties->u.tab.size;
      i++) {
    ryspec_toml_doc_entity p =
      entry(RYSPEC_ENTITY_PROPERTY, properties, i, ns, NULL);
    s = v->fn(&p, v->ctx, v->diag);
    if(s == RYSPEC_OK) {
      s = visit_parts(v, &p);
    }
  }
  const toml_datum_t* t = namespaces_of(v->doc, ns);
  for(int i = 0; s == RYSPEC_OK && is_table(t) && i < t->u.tab.size; i++) {
    if(!is_namespace(t, i)) {
      continue;
    }
    ryspec_toml_doc_entity e = entry(RYSPEC_ENTITY_NAMESPACE, t, i, ns, NULL);
    s = v->fn(&e, v->ctx, v->diag);
    if(s == RYSPEC_OK) {
      s = visit_namespace(v, e.value);
    }
  }
  return s;
}

int ryspec_toml_doc_each_entity(
  const ryspec_toml_doc* doc,
  ryspec_toml_doc_entity_fn fn,
  void* ctx,
  ryspec_diag* diag)
{
  const visit v = {doc, fn, ctx, diag};
  const toml_datum_t* root = ryspec_toml_doc_root(doc);
  int s = visit_namespace(&v, root);
  return s == RYSPEC_OK ? visit_table(
                            &v,
                            get(root, "variables"),
                            RYSPEC_ENTITY_VARIABLE,
                            true,
                            NULL,
                            NULL)
                        : s;
}

bool ryspec_toml_doc_is_position(ryspec_entity_kind kind)
{
  return kind == RYSPEC_ENTITY_RULE || kind == RYSPEC_ENTITY_PRIVATE_RULE ||
         kind == RYSPEC_ENTITY_GIVEN || kind == RYSPEC_ENTITY_CHECK;
}

/* The index of the key of len bytes at name in t, or -1 when t is no table
 * or holds no such key. */
static int find(const toml_datum_t* t, const char* name, size_t len)
{
  for(int i = 0; is_table(t) && i < t->u.tab.size; i++) {
    if(
      (size_t)t->u.tab.len[i] == len &&
      memcmp(t->u.tab.key[i], name, len) == 0) {
      return i;
    }
  }
  return -1;
}

const toml_datum_t* ryspec_toml_doc_variable(
  const ryspec_toml_doc* doc, const char* name, size_t len)
{
  const toml_datum_t* variables = get(ryspec_toml_doc_root(doc), "variables");
  int i = find(variables, name, len);
  return i >= 0 && is_table(&variables->u.tab.value[i])
           ? &variables->u.tab.value[i]
           : NULL;
}

ryspec_value_type ryspec_toml_doc_variable_type(const toml_datum_t* decl)
{
  const toml_datum_t* type = get(decl, "type");
  if(!type || type->type != TOML_STRING) {
    return RYSPEC_TYPE_BOOL;
  }
  if(strcmp(type->u.s, "number") == 0) {
    return RYSPEC_TYPE_NUMBER;
  }
  if(strcmp(type->u.s, "text") == 0) {
    return RYSPEC_TYPE_TEXT;
  }
  if(strcmp(type->u.s, "binary") == 0) {
    return RYSPEC_TYPE_BINARY;
  }
  return RYSPEC_TYPE_BOOL;
}

const char* ryspec_toml_doc_type_name(ryspec_value_type t)
{
  static const char* names[] = {"bool", "number", "text", "binary"};
  return names[t];
}

/* ---------------------------------------------------------------------------
 * Names. */

/* What a name answers to, so far: how many entities, and the first. */
typedef struct answers {
  int count;
  ryspec_toml_doc_entity first;
} answers;

/* Count the entry of len bytes at name in t, as an entity of kind in ns and
 * property, into *to; a namespace only where it is one. */
static void answer(
  const toml_datum_t* t,
  const char* name,
  size_t len,
  ryspec_entity_kind kind,
  const toml_datum_t* ns,
  const toml_datum_t* property,
  answers* to)
{
  int i = find(t, name, len);
  if(i < 0 || (kind == RYSPEC_ENTITY_NAMESPACE && !is_namespace(t, i))) {
    return;
  }
  if(!to->count++) {
    to->first = entry(kind, t, i, ns, property);
  }
}

static ryspec_toml_doc_target target(const answers* a, ryspec_resolution none)
{
  if(a->count == 0) {
    return (ryspec_toml_doc_target){none, {0}};
  }
  return (ryspec_toml_doc_target){
    a->count == 1 ? RYSPEC_RESOLVES_ENTITY : RYSPEC_RESOLVES_AMBIGUOUS,
    a->first};
}

/* Count the rules and properties of len bytes at name in the namespace ns
 * into *to. */
static void answer_logic(
  const toml_datum_t* ns, const char* name, size_t len, answers* to)
{
  answer(get(ns, "rules"), name, len, RYSPEC_ENTITY_RULE, ns, NULL, to);
  answer(
    get(ns, "properties"), name, len, RYSPEC_ENTITY_PROPERTY, ns, NULL, to);
}

static ryspec_toml_doc_target resolve_path(
  const ryspec_toml_doc* doc, const char* name, size_t len)
{
  const toml_datum_t* ns = ryspec_toml_doc_root(doc);
  const char* end = name + len;
  for(const char* dot; (dot = memchr(name, '.', (size_t)(end - name)));
      name = dot + 1) {
    answers a = {0};
    answer(
      namespaces_of(doc, ns),
      name,
      (size_t)(dot - name),
      RYSPEC_ENTITY_NAMESPACE,
      ns,
      NULL,
      &a);
    if(!a.count) {
      return (ryspec_toml_doc_target){RYSPEC_RESOLVES_NOTHING, {0}};
    }
    ns = a.first.value;
  }
  answers a = {0};
  answer_logic(ns, name, (size_t)(end - name), &a);
  if(!a.count) {
    answer(
      namespaces_of(doc, ns),
      name,
      (size_t)(end - name),
      RYSPEC_ENTITY_NAMESPACE,
      ns,
      NULL,
      &a);
  }
  return target(&a, RYSPEC_RESOLVES_NOTHING);
}

ryspec_toml_doc_target ryspec_toml_doc_resolve(
  const ryspec_toml_doc* doc,
  const ryspec_toml_doc_entity* at,
  const char* name,
  size_t len)
{
  if(memchr(name, '.', len)) {
    return resolve_path(doc, name, len);
  }
  const toml_datum_t* root = ryspec_toml_doc_root(doc);
  /* The namespace and property at is in, or is. */
  const toml_datum_t *ns = root, *property = NULL;
  if(at && at->kind == RYSPEC_ENTITY_NAMESPACE) {
    ns = at->value;
  }
  else if(at && at->ns) {
    ns = at->ns;
    property = at->kind == RYSPEC_ENTITY_PROPERTY ? at->value : at->property;
  }
  answers a = {0};
  answer(
    get(property, "where"),
    name,
    len,
    RYSPEC_ENTITY_PRIVATE_RULE,
    ns,
    property,
    &a);
  answer_logic(ns, name, len, &a);
  if(ns == root) {
    answer(
      namespaces_of(doc, ns), name, len, RYSPEC_ENTITY_NAMESPACE, ns, NULL, &a);
  }
  const toml_datum_t* variables = get(root, "variables");
  int i = find(variables, name, len);
  if(i >= 0 && is_table(&variables->u.tab.value[i]) && !a.count++) {
    a.first = entry(RYSPEC_ENTITY_VARIABLE, variables, i, NULL, NULL);
  }
  return target(&a, RYSPEC_RESOLVES_DEDUCED);
}

ryspec_entity_kind ryspec_toml_doc_target_kind(ryspec_toml_doc_target t)
{
  return t.resolution == RYSPEC_RESOLVES_ENTITY ? t.entity.kind
                                                : RYSPEC_ENTITY_NONE;
}

bool ryspec_toml_doc_target_type(
  ryspec_toml_doc_target t, ryspec_value_type* out)
{
  switch(ryspec_toml_doc_target_kind(t)) {
    case RYSPEC_ENTITY_VARIABLE:
      *out = ryspec_toml_doc_variable_type(t.entity.value);
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
