/*
 * Documents, private to libryspec: the one place tomlc17's types meet the
 * library's. The public header names none: a ryspec_toml_doc is the
 * block below.
 *
 * A document is tomlc17's tree, as parsed, but for its expressions: each
 * expression-form string at a rule position is translated to prefix-form
 * TOML, which tomlc17 parses into a second result, and the array it gives
 * replaces the string in the tree (translate.c). The twins' strings are
 * copied into the document's block, after the document, and the second
 * result freed: nothing in the tree points at the expression form.
 *
 * The index: the document's entities in one table, one id space, and the
 * name index that resolves references over them, built once the document is
 * translated, as fields of it, and released with it. Names, places and nodes
 * point into tomlc17's tree, copying nothing. Values of the wrong shape are
 * passed over, as are `extras` tables.
 *
 * Ids are dense, the root namespace 0, in the document's order: a
 * namespace, its rules, then for each of its properties the property, its
 * `given`, its `check` and its private rules, then its named namespaces,
 * each so in turn; then the variables; then the monitors. An entity's
 * parent is the only structural link: a namespace's is the namespace that
 * holds it, the root's none; a rule's and a property's, their namespace; a
 * private rule's, a `given`'s and a `check`'s, their property; a variable's
 * and a monitor's, none. A `given` and a `check` are positions, unnamed.
 *
 * The name index is the specification's symbol space: what a reference can
 * resolve to, each name in a scope -- a namespace, rule or property in its
 * parent namespace, a private rule in its property, a variable in the
 * global scope. A monitor is a runtime interface over that space, outside
 * it: its entries are references into it, and its own name is no symbol.
 */
#ifndef RYSPEC_SRC_TOML_DOC_H
#define RYSPEC_SRC_TOML_DOC_H

#include <stdbool.h>
#include <stddef.h>

#include "ryspec/ryspec.h"
#include "tomlc17.h"

/* The entity kinds and ids are public (ryspec.h), read through the
 * document; the rest stays here until a consumer needs it. */

typedef enum ryspec_value_type {
  RYSPEC_TYPE_BOOL,
  RYSPEC_TYPE_NUMBER,
  RYSPEC_TYPE_TEXT,
  RYSPEC_TYPE_BINARY,
} ryspec_value_type;

typedef enum ryspec_resolution {
  RYSPEC_RESOLVES_NOTHING,   /* a dotted path nothing answers to */
  RYSPEC_RESOLVES_DEDUCED,   /* a bare name nothing declares: an input */
  RYSPEC_RESOLVES_ENTITY,    /* exactly one entity */
  RYSPEC_RESOLVES_AMBIGUOUS, /* more than one: Rule 11's */
} ryspec_resolution;

typedef enum ryspec_monitor_list {
  RYSPEC_MONITOR_INPUTS,
  RYSPEC_MONITOR_PARAMETERS,
  RYSPEC_MONITOR_OUTPUTS,
} ryspec_monitor_list;

/* An entity. name is NULL for the root, a `given` and a `check`; line and
 * column are 0 where the entity has no place. */
typedef struct index_entity {
  ryspec_entity_kind kind;
  ryspec_entity parent;
  const char *name;
  int len;
  int line, column;
  union {
    ryspec_value_type type; /* a variable's */
    struct {
      ryspec_entity given, check; /* RYSPEC_NO_ENTITY where absent */
    } property;
  } u;
} index_entity;

/* A record of the name index, by (scope, name, entity). The global scope
 * is RYSPEC_NO_ENTITY. */
typedef struct index_name {
  ryspec_entity scope, entity;
  const char *name;
  int len;
} index_name;

struct ryspec_toml_doc {
  toml_result_t result; /* the document, its expressions translated */

  /* Its index, built once it is translated. */
  index_entity *entities;
  const toml_datum_t **nodes; /* per entity, the tree node the linter reads */
  size_t n_entities, cap_entities;
  index_name *names; /* the name index, sorted by (scope, name, entity) */
  size_t n_names;
};

/* The value of the key of key_len bytes in table t, or NULL when t is no
 * table or has no such key. */
toml_datum_t *ryspec_toml_value_lookup(const toml_datum_t *t, const char *key,
                                       size_t key_len);

/* Set tomlc17's options for a parse, returning them: its allocator, which
 * the library allocates through too, and the UTF-8 check. */
toml_option_t ryspec_toml_options(void);

/* Fill diag, which may be NULL, with status at line and column, and the
 * message fmt formats. */
void ryspec_diagnose(ryspec_diagnostic *diag, ryspec_status status, int line,
                     int column, const char *fmt, ...);

/* Classify a failed result's errmsg into diag, which may be NULL, as a
 * TOML status at the line it names, the message without the position. */
void ryspec_toml_error(const char *errmsg, ryspec_diagnostic *diag);

/* Search (*doc)->result for expression-form strings at rule positions and
 * replace each by its prefix twin, *doc's block grown, and so perhaps
 * moved, to hold the twins' strings after the document. On failure, *doc is
 * only fit to be freed. Returns RYSPEC_OK, or the status diag, which may be
 * NULL, is filled with: RYSPEC_ERROR_GRAMMAR at the first expression that
 * does not parse, placed in the document, the TOML status of a twin
 * tomlc17 refuses, or RYSPEC_ERROR_MEMORY. */
ryspec_status ryspec_translate(ryspec_toml_doc **doc, toml_option_t opt,
                               ryspec_diagnostic *diag);

/* ---------------------------------------------------------------------------
 * The index. */

/* Index doc, whose index is empty. Returns RYSPEC_OK, or
 * RYSPEC_ERROR_MEMORY with diag, which may be NULL, filled and doc's index
 * only fit to be released. */
ryspec_status index_init(ryspec_toml_doc *doc, ryspec_diagnostic *diag);

/* Release doc's index, leaving it empty; an empty index may be released
 * again. */
void index_release(ryspec_toml_doc *doc);

/* The entity e, or NULL when e is no id of doc. */
const index_entity *index_at(const ryspec_toml_doc *doc, ryspec_entity e);

/* The kind of e, RYSPEC_ENTITY_NONE when e is no id of doc. */
ryspec_entity_kind index_kind(const ryspec_toml_doc *doc, ryspec_entity e);

/* The tree node of e: a rule position's rule, a variable's declaration, a
 * namespace's, property's or monitor's table; NULL when e is no id. */
const toml_datum_t *index_node(const ryspec_toml_doc *doc, ryspec_entity e);

/* The first entity of kind from e on, in id order, or doc->n_entities:
 * so
 *   for (e = index_next(doc, 0, k); e < doc->n_entities;
 *        e = index_next(doc, e + 1, k))
 * visits every entity of kind k. */
ryspec_entity index_next(const ryspec_toml_doc *doc, ryspec_entity e,
                         ryspec_entity_kind kind);

/* Whether kind is a rule position: a rule, private rule, given or check. */
bool index_is_position(ryspec_entity_kind kind);

/* The nearest namespace holding e, or e itself if a namespace; the root for
 * RYSPEC_NO_ENTITY. */
ryspec_entity index_namespace_of(const ryspec_toml_doc *doc, ryspec_entity e);

/* The nearest property holding e, or e itself if a property, or
 * RYSPEC_NO_ENTITY. */
ryspec_entity index_property_of(const ryspec_toml_doc *doc, ryspec_entity e);

/* The name records of len bytes at name in scope, *count of them, sorted
 * by entity; NULL with *count 0 when there are none. */
const index_name *index_lookup(const ryspec_toml_doc *doc, ryspec_entity scope,
                               const char *name, size_t len, size_t *count);

/* What a reference resolves to, and the entity: the one for
 * RYSPEC_RESOLVES_ENTITY, the first by id for RYSPEC_RESOLVES_AMBIGUOUS,
 * RYSPEC_NO_ENTITY otherwise. */
typedef struct index_target {
  ryspec_resolution resolution;
  ryspec_entity entity;
} index_target;

/* What the len bytes at name, written at the entity at, resolve to. A bare
 * name is looked up among the private rules of at's property, the rules
 * and properties of at's namespace, and at the root its top-level
 * namespaces, and the variables; a dotted path walks from the root, each
 * segment but the last a namespace. at RYSPEC_NO_ENTITY is the root. */
index_target index_resolve(const ryspec_toml_doc *doc, ryspec_entity at,
                           const char *name, size_t len);

/* The kind of the one entity t resolves to, or RYSPEC_ENTITY_NONE. */
ryspec_entity_kind index_target_kind(const ryspec_toml_doc *doc,
                                     index_target t);

/* The type a reference has, when it resolves to a variable, a rule or a
 * property, a rule's or property's being RYSPEC_TYPE_BOOL, in *out,
 * returning whether it has one. */
bool index_target_type(const ryspec_toml_doc *doc, index_target t,
                       ryspec_value_type *out);

/* The name of the type, as "text". */
const char *index_type_name(ryspec_value_type t);

/* The variable declared as the len bytes at name, or RYSPEC_NO_ENTITY. */
ryspec_entity index_variable_find(const ryspec_toml_doc *doc, const char *name,
                                  size_t len);

/* The monitor of len bytes at name, or RYSPEC_NO_ENTITY. */
ryspec_entity index_monitor_find(const ryspec_toml_doc *doc, const char *name,
                                 size_t len);

/* The monitor m's list, or NULL when it lacks one or it is no array. */
const toml_datum_t *index_monitor_list(const ryspec_toml_doc *doc,
                                       ryspec_entity m,
                                       ryspec_monitor_list list);

/* Whether the array a, which may be NULL, holds the string of len bytes at
 * name. */
bool index_array_holds(const toml_datum_t *a, const char *name, size_t len);

/* Whether monitor m lists the len bytes at name in any of its lists. */
bool index_monitor_lists(const ryspec_toml_doc *doc, ryspec_entity m,
                         const char *name, size_t len);

#endif /* RYSPEC_SRC_TOML_DOC_H */
