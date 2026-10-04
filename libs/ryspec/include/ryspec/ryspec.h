/*
 * ryspec: reading ryspec specification documents.
 *
 * A document is parsed from a buffer or a file into an opaque
 * ryspec_toml_doc, which the caller releases with
 * ryspec_toml_doc_free(). A failed parse returns NULL and fills a
 * ryspec_diagnostic saying where and why.
 *
 * A document is its TOML with one change: every rule written in expression
 * form is replaced, as it is parsed, by its twin in prefix form, so a rule is
 * always a prefix-form value -- a string naming a rule or variable, or an
 * array [op, operands..., kwargs].
 *
 * A document is indexed as it is parsed, and linted: its entities --
 * namespaces, rules, properties, variables, monitors -- are read through it
 * by id, as kind, name, parent and place, never their values.
 */
#ifndef RYSPEC_RYSPEC_H
#define RYSPEC_RYSPEC_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What went wrong, by layer: TOML, the document's schema, the grammar of
 * expression form, then the rules the schema cannot check. The TOML errors
 * are those the TOML parser tells apart, and the diagnostic's message is the
 * parser's own. */
typedef enum ryspec_status {
  RYSPEC_OK = 0,
  RYSPEC_ERROR_IO,             /* the file could not be read */
  RYSPEC_ERROR_MEMORY,         /* an allocation failed */
  RYSPEC_ERROR_TOML,           /* not valid TOML: a syntax error */
  RYSPEC_ERROR_TOML_ENCODING,  /* not valid TOML: a character that is no
                                  Unicode scalar value, as \uD800 */
  RYSPEC_ERROR_TOML_REDEFINED, /* not valid TOML: a key or table defined
                                  twice, or an inline table or static array
                                  extended */
  RYSPEC_ERROR_TOML_LIMIT,     /* maybe valid TOML, but past what the parser
                                  holds: nesting, key parts or entries */
  RYSPEC_ERROR_SCHEMA,         /* valid TOML, but not a ryspec document */
  RYSPEC_ERROR_GRAMMAR,        /* schema-valid, but an expression-form string
                                  the grammar of SPEC.md does not derive */
  RYSPEC_ERROR_SEMANTIC,       /* schema-valid, but breaks a rule of SPEC.md */
} ryspec_status;

typedef struct ryspec_diagnostic {
  ryspec_status status;
  int line;   /* 1-based, or 0 when unknown */
  int column; /* 1-based, or 0 when unknown */
  char message[256];
} ryspec_diagnostic;

typedef struct ryspec_toml_doc ryspec_toml_doc;

/* The library version, as "MAJOR.MINOR.PATCH". */
const char *ryspec_version(void);

/* Parse len bytes of src; src need not be NUL-terminated. name labels the
 * input in diagnostics and may be NULL. diag may be NULL. Fails with
 * RYSPEC_ERROR_GRAMMAR, placed in the document, at the first expression the
 * grammar does not derive, and with RYSPEC_ERROR_MEMORY when an allocation
 * fails. */
ryspec_toml_doc *ryspec_toml_parse(const char *src, size_t len,
                                   const char *name, ryspec_diagnostic *diag);

/* Parse the file at path. diag may be NULL. */
ryspec_toml_doc *ryspec_toml_parse_file(const char *path,
                                        ryspec_diagnostic *diag);

/* Release a document. NULL is ignored. */
void ryspec_toml_doc_free(ryspec_toml_doc *doc);

/* What the linter checks: the schema, as 0, then the rules of SPEC.md's
 * "What the schema cannot check", each named by its number there. */
typedef enum ryspec_lint_rule {
  RYSPEC_LINT_SCHEMA = 0,                  /* schemas/v0/, by hand */
  RYSPEC_LINT_MONITOR_LIST_OVERLAP = 1,    /* a name in one monitor list */
  RYSPEC_LINT_MONITOR_ENTRY_RESOLVES = 3,  /* every monitor entry resolves */
  RYSPEC_LINT_INITIAL_VALUE_LISTED = 5,    /* initial_value on listed names */
  RYSPEC_LINT_PARAMETER_INITIAL = 6,       /* a parameter has initial_value */
  RYSPEC_LINT_MIN_LE_MAX = 7,              /* min no greater than max */
  RYSPEC_LINT_SOURCE_HEAD = 8,             /* a source's head has a format */
  RYSPEC_LINT_NO_TEXT_AS_TRUTH = 9,        /* text, binary never a truth or
                                              a number */
  RYSPEC_LINT_ONE_SOURCE_OF_VALUE = 11,    /* one source per visible name */
  RYSPEC_LINT_ONE_CONE = 12,               /* no rule mixes time cones */
  RYSPEC_LINT_NO_CYCLE = 13,               /* no rule defined by itself */
  RYSPEC_LINT_QVAR_BOUND = 14,             /* a qvar has its quantifier */
  RYSPEC_LINT_QVAR_NO_REBIND = 15,         /* no quantifier rebinds a name */
  RYSPEC_LINT_QVAR_NO_SHADOW = 16,         /* no qvar takes a declared name */
  RYSPEC_LINT_QVAR_USED = 17,              /* every bound name is used */
  RYSPEC_LINT_PARAMETER_NO_SOURCE = 18,    /* a parameter has no source */
  RYSPEC_LINT_BOUND_NAME_NUMBER = 19,      /* a bound name is a number
                                              parameter */
  RYSPEC_LINT_COMPARISON_TYPES = 20,       /* operands fit the comparison */
  RYSPEC_LINT_NO_NAMESPACE_REFERENCE = 22, /* no name is a namespace */
  RYSPEC_LINT_NO_NAN = 23,                 /* no number is nan */
} ryspec_lint_rule;

/* The rules ryspec_toml_lint() checks, in the order it checks them; *count
 * is set to how many. */
const ryspec_lint_rule *ryspec_lint_rules(size_t *count);

/* Lint doc: check it against the schema, then the rules the schema cannot
 * check, in ryspec_lint_rules()' order; nothing under an `extras` table is
 * checked. Returns the status diag, which may be NULL, is filled with:
 * RYSPEC_OK; at the first violation, placed in the document,
 * RYSPEC_ERROR_SCHEMA for the schema's and RYSPEC_ERROR_SEMANTIC for a
 * rule's; or RYSPEC_ERROR_MEMORY. */
ryspec_status ryspec_toml_lint(const ryspec_toml_doc *doc,
                               ryspec_diagnostic *diag);

/* Lint doc against the one rule, as ryspec_toml_lint() does. A rule other
 * than the schema presumes a document the schema accepts. A rule the linter
 * does not check fails with RYSPEC_ERROR_SEMANTIC, unplaced. */
ryspec_status ryspec_toml_lint_rule(const ryspec_toml_doc *doc,
                                    ryspec_lint_rule rule,
                                    ryspec_diagnostic *diag);

/* ---------------------------------------------------------------------------
 * The entities of a document, each an id, indexed as it is parsed.
 *
 * Ids are dense, from 0, the root namespace, to ryspec_entity_count() less
 * one, in the document's order: a namespace, its rules, then for each of its
 * properties the property, its `given`, its `check` and the rules of its
 * `where`; then its named namespaces, each so in turn; then the variables;
 * then the monitors. An entity's parent is the namespace that holds a
 * namespace, rule or property, the property that holds a private rule, a
 * `given` or a `check`, and none for the root, a variable or a monitor. A
 * `given` and a `check` are positions: unnamed, and never what a name
 * resolves to. A monitor is outside the namespaces: an interface over them,
 * whose name is no name a rule can use.
 *
 * Every function below is total. An id that is RYSPEC_NO_ENTITY or past
 * the last, or a document that is NULL, which holds none, reads as no
 * entity: kind RYSPEC_ENTITY_NONE, parent RYSPEC_NO_ENTITY, no name, no
 * path and no place. An out-pointer may be NULL. */

/* An entity of a document: an id below ryspec_entity_count(). */
typedef size_t ryspec_entity;
#define RYSPEC_NO_ENTITY ((ryspec_entity)-1)

typedef enum ryspec_entity_kind {
  RYSPEC_ENTITY_NONE = 0, /* no entity: what an invalid id reads as */
  RYSPEC_ENTITY_NAMESPACE,
  RYSPEC_ENTITY_RULE,
  RYSPEC_ENTITY_PROPERTY,
  RYSPEC_ENTITY_PRIVATE_RULE, /* a rule of a property's `where` */
  RYSPEC_ENTITY_VARIABLE,
  RYSPEC_ENTITY_MONITOR,
  RYSPEC_ENTITY_GIVEN, /* a property's `given`: a position */
  RYSPEC_ENTITY_CHECK, /* a property's `check`: a position */
} ryspec_entity_kind;

/* How many entities doc holds. Values of the wrong shape are passed over,
 * so lint a document to see every entity it seems to hold. */
size_t ryspec_entity_count(const ryspec_toml_doc *doc);

ryspec_entity_kind ryspec_entity_kind_of(const ryspec_toml_doc *doc,
                                         ryspec_entity entity);

ryspec_entity ryspec_entity_parent(const ryspec_toml_doc *doc,
                                   ryspec_entity entity);

/* The entity's name, its length in *len; not NUL-terminated. NULL, with
 * *len 0, for an unnamed entity: the root, a `given`, a `check`. */
const char *ryspec_entity_name(const ryspec_toml_doc *doc, ryspec_entity entity,
                               size_t *len);

/* Write the entity's path into buf, NUL-terminated and cut to size bytes,
 * as snprintf() does; buf may be NULL when size is 0. Returns the path's
 * full length, 0 for an unnamed entity. The path names the entity: its
 * namespaces' names, then its property's for a private rule, then its own,
 * joined by dots, as `a.b.p.r`. It is no reference: what a name resolves
 * to, SPEC.md's "Names" says, and no reference reaches a private rule from
 * outside its property. */
size_t ryspec_entity_path(const ryspec_toml_doc *doc, ryspec_entity entity,
                          char *buf, size_t size);

/* The entity's place in the document, 1-based, in *line and *column,
 * returning whether it has one; 0 and 0 when not. */
bool ryspec_entity_place(const ryspec_toml_doc *doc, ryspec_entity entity,
                         int *line, int *column);

#ifdef __cplusplus
}
#endif

#endif /* RYSPEC_RYSPEC_H */
