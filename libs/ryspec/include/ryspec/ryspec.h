/*
 * ryspec: reading ryspec specification documents.
 *
 * A document is parsed from a buffer or a file into an opaque
 * ryspec_toml_doc, which the caller releases with
 * ryspec_toml_doc_free(). A failed parse returns NULL and fills a
 * ryspec_diag saying where and why.
 *
 * A document is its TOML with one change: every rule written in expression
 * form is replaced, as it is parsed, by its twin in prefix form, so a rule is
 * always a prefix-form value -- a string naming a rule or variable, or an
 * array [op, operands..., kwargs].
 *
 * A document is linted: checked against the schema and the rules the schema
 * cannot check.
 */
#ifndef RYSPEC_RYSPEC_H
#define RYSPEC_RYSPEC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What went wrong, by layer: TOML, the document's schema, the grammar of
 * expression form, then the rules the schema cannot check. The TOML errors
 * are those the TOML parser tells apart, and the diagnostic's message is the
 * parser's own. */
enum {
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
};

typedef struct ryspec_diag {
  int status; /* a RYSPEC_OK or RYSPEC_ERROR_* */
  int line;   /* 1-based, or 0 when unknown */
  int column; /* 1-based, or 0 when unknown */
  char message[256];
} ryspec_diag;

typedef struct ryspec_toml_doc ryspec_toml_doc;

/* Version: MAJOR * 100000 + MINOR * 100 + PATCH, so 0.1.0 is 100. */
int ryspec_version(void);

ryspec_toml_doc* ryspec_toml_parse(
  const char* src, size_t len, ryspec_diag* diag);

ryspec_toml_doc* ryspec_toml_parse_file(const char* path, ryspec_diag* diag);

int ryspec_toml_lint(const ryspec_toml_doc* doc, ryspec_diag* diag);

/* Release a document. NULL is ignored. */
void ryspec_toml_doc_free(ryspec_toml_doc* doc);

#ifdef __cplusplus
}
#endif

#endif /* RYSPEC_RYSPEC_H */
