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
 * grammar does not derive. */
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

#ifdef __cplusplus
}
#endif

#endif /* RYSPEC_RYSPEC_H */
