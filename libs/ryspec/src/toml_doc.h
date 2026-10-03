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
 */
#ifndef RYSPEC_SRC_TOML_DOC_H
#define RYSPEC_SRC_TOML_DOC_H

#include <stddef.h>

#include "ryspec/ryspec.h"
#include "tomlc17.h"

struct ryspec_toml_doc {
  toml_result_t result; /* the document, its expressions translated */
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

#endif /* RYSPEC_SRC_TOML_DOC_H */
