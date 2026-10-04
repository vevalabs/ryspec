/*
 * Documents, private to libryspec: the one place tomlc17's types meet the
 * library's. The public header names none: a ryspec_toml_doc is the block
 * below.
 *
 * A document is tomlc17's tree, as parsed, but for its expressions: each
 * expression-form string at a rule position is translated to prefix-form
 * TOML, which tomlc17 parses into a second result, and the array it gives
 * replaces the string in the tree (translate.c). The twins' strings are
 * copied into the document's block, after the document, and the second
 * result freed: nothing in the tree points at the expression form.
 *
 * Nothing else is kept: no index. The document's entities -- namespaces,
 * rules, properties, their `given`s, `check`s and private rules, and
 * variables -- are read from the tree where they stand, by a visitor that
 * walks them in the document's order, and a reference is resolved, by
 * SPEC.md's "Names", with a few lookups in the tables that declare names.
 * TOML gives a table no key twice, so a scope holds a name at most once per
 * table, and a name more than one table holds where a reference stands is
 * ambiguous. Values of the wrong shape are passed over, as are `extras`
 * tables. A monitor is no entity: a runtime interface over the
 * specification's names, outside them, its entries references into them
 * and its own name none.
 */
#ifndef RYSPEC_SRC_TOML_DOC_H
#define RYSPEC_SRC_TOML_DOC_H

#include <stdbool.h>
#include <stddef.h>

#include "ryspec/ryspec.h"

#include "tomlc17.h"

struct ryspec_toml_doc {
  toml_result_t result; /* the document, its expressions translated */
};

/* The value of the key of key_len bytes in table t, or NULL when t is no
 * table or has no such key. */
toml_datum_t* ryspec_toml_value_lookup(
  const toml_datum_t* t, const char* key, size_t key_len);

/* Set tomlc17's options for a parse, returning them: its allocator, which
 * the library allocates through too, and the UTF-8 check. */
toml_option_t ryspec_toml_options(void);

/* Fill diag, which may be NULL, with status at line and column, and the
 * message fmt formats. */
void ryspec_diagnose(
  ryspec_diag* diag, int status, int line, int column, const char* fmt, ...);

/* Classify a failed result's errmsg into diag, which may be NULL, as a
 * TOML status at the line it names, the message without the position. */
void ryspec_toml_error(const char* errmsg, ryspec_diag* diag);

/* Search (*doc)->result for expression-form strings at rule positions and
 * replace each by its prefix twin, *doc's block grown, and so perhaps
 * moved, to hold the twins' strings after the document. On failure, *doc is
 * only fit to be freed. Returns RYSPEC_OK, or the status diag, which may be
 * NULL, is filled with: RYSPEC_ERROR_GRAMMAR at the first expression that
 * does not parse, placed in the document, the TOML status of a twin
 * tomlc17 refuses, or RYSPEC_ERROR_MEMORY. */
int ryspec_translate(
  ryspec_toml_doc** doc, toml_option_t opt, ryspec_diag* diag);

/* ---------------------------------------------------------------------------
 * Entities. */

typedef enum ryspec_entity_kind {
  RYSPEC_ENTITY_NONE = 0,
  RYSPEC_ENTITY_NAMESPACE,
  RYSPEC_ENTITY_RULE,
  RYSPEC_ENTITY_PROPERTY,
  RYSPEC_ENTITY_PRIVATE_RULE, /* a rule of a property's `where` */
  RYSPEC_ENTITY_VARIABLE,
  RYSPEC_ENTITY_GIVEN, /* a property's `given`: a position */
  RYSPEC_ENTITY_CHECK, /* a property's `check`: a position */
} ryspec_entity_kind;

typedef enum ryspec_value_type {
  RYSPEC_TYPE_BOOL,
  RYSPEC_TYPE_NUMBER,
  RYSPEC_TYPE_TEXT,
  RYSPEC_TYPE_BINARY,
} ryspec_value_type;

/* An entity, as it stands in the tree: its kind, its key, its value -- a
 * rule position's rule, a variable's declaration, a namespace's or
 * property's table -- and the tables holding it. A `given` and a `check`
 * are positions, unnamed, and carry their property's key, for a
 * diagnostic. ns is the namespace's table it is in, the root's being the
 * document's top table, and for a namespace the one holding it; NULL for a
 * variable. property is the property's table holding a private rule, a
 * `given` or a `check`, and NULL otherwise. Two entities are one when their
 * values are. */
typedef struct ryspec_toml_doc_entity {
  ryspec_entity_kind kind;
  const char* name;
  int len;
  const toml_datum_t* value;
  const toml_datum_t* ns;
  const toml_datum_t* property;
} ryspec_toml_doc_entity;

/* The document's top table: the root namespace's. */
const toml_datum_t* ryspec_toml_doc_root(const ryspec_toml_doc* doc);

/* Receives each entity; anything but RYSPEC_OK stops the visit and is
 * returned. */
typedef int (*ryspec_toml_doc_entity_fn)(
  const ryspec_toml_doc_entity* e, void* ctx, ryspec_diag* diag);

/* Visit the entities of doc in the document's order: a namespace's rules,
 * then for each of its properties the property, its `given`, its `check`
 * and its private rules, then each of its named namespaces and what it
 * holds, so in turn, from the root; then the variables. The root is
 * visited as no entity. */
int ryspec_toml_doc_each_entity(
  const ryspec_toml_doc* doc,
  ryspec_toml_doc_entity_fn fn,
  void* ctx,
  ryspec_diag* diag);

/* Whether kind is a rule position's: a rule, private rule, given or
 * check. */
bool ryspec_toml_doc_is_position(ryspec_entity_kind kind);

/* The `given` or `check`, as part, of the property p, in *out, returning
 * whether it has one. */
bool ryspec_toml_doc_property_part(
  const ryspec_toml_doc_entity* p,
  ryspec_entity_kind part,
  ryspec_toml_doc_entity* out);

/* The declaration of the variable of len bytes at name, or NULL. */
const toml_datum_t* ryspec_toml_doc_variable(
  const ryspec_toml_doc* doc, const char* name, size_t len);

/* The type the variable declaration decl declares; RYSPEC_TYPE_BOOL where
 * it names none. */
ryspec_value_type ryspec_toml_doc_variable_type(const toml_datum_t* decl);

/* The name of the type, as "text". */
const char* ryspec_toml_doc_type_name(ryspec_value_type t);

/* ---------------------------------------------------------------------------
 * Names. */

typedef enum ryspec_resolution {
  RYSPEC_RESOLVES_NOTHING,   /* a dotted path nothing answers to */
  RYSPEC_RESOLVES_DEDUCED,   /* a bare name nothing declares: an input */
  RYSPEC_RESOLVES_ENTITY,    /* exactly one entity */
  RYSPEC_RESOLVES_AMBIGUOUS, /* more than one: Rule 11's */
} ryspec_resolution;

/* What a reference resolves to, and the entity: the one, or for
 * RYSPEC_RESOLVES_AMBIGUOUS one of them; of kind RYSPEC_ENTITY_NONE
 * otherwise. */
typedef struct ryspec_toml_doc_target {
  ryspec_resolution resolution;
  ryspec_toml_doc_entity entity;
} ryspec_toml_doc_target;

/* What the len bytes at name, written at the entity at, resolve to. A bare
 * name is looked up among the private rules of at's property, the rules
 * and properties of at's namespace, and at the root its top-level
 * namespaces, and the variables; a dotted path walks from the root, each
 * segment but the last a namespace. at NULL is the root. */
ryspec_toml_doc_target ryspec_toml_doc_resolve(
  const ryspec_toml_doc* doc,
  const ryspec_toml_doc_entity* at,
  const char* name,
  size_t len);

/* The kind of the one entity t resolves to, or RYSPEC_ENTITY_NONE. */
ryspec_entity_kind ryspec_toml_doc_target_kind(ryspec_toml_doc_target t);

/* The type a reference has, when it resolves to a variable, a rule or a
 * property, a rule's or property's being RYSPEC_TYPE_BOOL, in *out,
 * returning whether it has one. */
bool ryspec_toml_doc_target_type(
  ryspec_toml_doc_target t, ryspec_value_type* out);

#endif /* RYSPEC_SRC_TOML_DOC_H */
