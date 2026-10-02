/*
 * ryspec: reading ryspec specification documents.
 *
 * A document is parsed from a buffer or a file into an opaque
 * ryspec_document, which the caller releases with ryspec_document_free().
 * A failed parse returns NULL and fills a ryspec_diagnostic saying where and
 * why.
 */
#ifndef RYSPEC_RYSPEC_H
#define RYSPEC_RYSPEC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What went wrong, by layer: TOML syntax, the document's schema, then the
 * rules the schema cannot check. */
typedef enum ryspec_status {
  RYSPEC_OK = 0,
  RYSPEC_ERROR_IO,       /* the file could not be read */
  RYSPEC_ERROR_MEMORY,   /* an allocation failed */
  RYSPEC_ERROR_TOML,     /* the input is not valid TOML */
  RYSPEC_ERROR_SCHEMA,   /* valid TOML, but not a ryspec document */
  RYSPEC_ERROR_SEMANTIC, /* schema-valid, but breaks a rule of SPEC.md */
} ryspec_status;

typedef struct ryspec_diagnostic {
  ryspec_status status;
  int line;   /* 1-based, or 0 when unknown */
  int column; /* 1-based, or 0 when unknown */
  char message[256];
} ryspec_diagnostic;

typedef struct ryspec_document ryspec_document;

/* The library version, as "MAJOR.MINOR.PATCH". */
const char *ryspec_version(void);

/* Parse len bytes of src; src need not be NUL-terminated. name labels the
 * input in diagnostics and may be NULL. diag may be NULL. */
ryspec_document *ryspec_parse(const char *src, size_t len, const char *name,
                              ryspec_diagnostic *diag);

/* Parse the file at path. diag may be NULL. */
ryspec_document *ryspec_parse_file(const char *path, ryspec_diagnostic *diag);

/* Release a document. NULL is ignored. */
void ryspec_document_free(ryspec_document *doc);

/* The document's format version, the value of its top-level `version`.
 * Owned by the document. */
const char *ryspec_document_version(const ryspec_document *doc);

/* Receives one finding of ryspec_lint(). diag is valid only during the call. */
typedef void (*ryspec_lint_fn)(const ryspec_diagnostic *diag, void *ctx);

/* Check doc against the rules of SPEC.md's "What the schema cannot check",
 * passing each violation, with status RYSPEC_ERROR_SEMANTIC, to report along
 * with ctx. report may be NULL. Returns the number of violations.
 *
 * No rule is checked yet, so this reports nothing and returns 0. */
size_t ryspec_lint(const ryspec_document *doc, ryspec_lint_fn report,
                   void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* RYSPEC_RYSPEC_H */
