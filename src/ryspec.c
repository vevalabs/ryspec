#include "ryspec/ryspec.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tomlc17.h"

struct ryspec_document {
  toml_result_t toml;
  const char *version;
};

static void diagnose(ryspec_diagnostic *diag, ryspec_status status, int line,
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

/* Takes ownership of toml. Only `version` is checked so far; the rest of the
 * schema is still to come. */
static ryspec_document *build(toml_result_t toml, ryspec_diagnostic *diag) {
  if (!toml.ok) {
    diagnose(diag, RYSPEC_ERROR_TOML, 0, 0, "%s", toml.errmsg);
    toml_free(toml);
    return NULL;
  }

  toml_datum_t version = toml_get(toml.toptab, "version");
  if (version.type == TOML_UNKNOWN) {
    diagnose(diag, RYSPEC_ERROR_SCHEMA, 1, 1, "missing required key `version`");
    toml_free(toml);
    return NULL;
  }
  if (version.type != TOML_STRING || strcmp(version.u.s, "0") != 0) {
    diagnose(diag, RYSPEC_ERROR_SCHEMA, version.lineno, version.colno,
             "`version` must be the string \"0\"");
    toml_free(toml);
    return NULL;
  }

  ryspec_document *doc = malloc(sizeof *doc);
  if (!doc) {
    diagnose(diag, RYSPEC_ERROR_MEMORY, 0, 0, "out of memory");
    toml_free(toml);
    return NULL;
  }
  doc->toml = toml;
  doc->version = version.u.s;
  diagnose(diag, RYSPEC_OK, 0, 0, "");
  return doc;
}

ryspec_document *ryspec_parse(const char *src, size_t len, const char *name,
                              ryspec_diagnostic *diag) {
  if (len > (size_t)INT_MAX) {
    diagnose(diag, RYSPEC_ERROR_IO, 0, 0, "input too large");
    return NULL;
  }
  return build(toml_parse_named(src, (int)len, name), diag);
}

ryspec_document *ryspec_parse_file(const char *path, ryspec_diagnostic *diag) {
  FILE *fp = fopen(path, "rb");
  if (!fp) {
    diagnose(diag, RYSPEC_ERROR_IO, 0, 0, "cannot open %s", path);
    return NULL;
  }
  toml_result_t toml = toml_parse_file_named(fp, path);
  fclose(fp);
  return build(toml, diag);
}

void ryspec_document_free(ryspec_document *doc) {
  if (!doc) {
    return;
  }
  toml_free(doc->toml);
  free(doc);
}

const char *ryspec_document_version(const ryspec_document *doc) {
  return doc->version;
}

size_t ryspec_lint(const ryspec_document *doc, ryspec_lint_fn report,
                   void *ctx) {
  (void)doc;
  (void)report;
  (void)ctx;
  return 0;
}
