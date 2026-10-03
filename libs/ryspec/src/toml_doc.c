/* Documents: parsing with tomlc17, and the translation of expressions. */
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
 * schema still to come, and its expressions translated. NULL, with diag
 * filled, when it fails. */
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
  if (doc && ryspec_translate(&doc, opt, diag) != RYSPEC_OK) {
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
  toml_free(doc->result);
  /* The allocator a block came from: tomlc17's default, which every parse
   * sets again (ryspec_toml_options()). */
  ryspec_toml_options().mem_free(doc);
}
