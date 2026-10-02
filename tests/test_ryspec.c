/* Smoke tests for libryspec. The one argument is the repository root, so the
 * documented examples can be parsed from it. */
#include "ryspec/ryspec.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
      failures++;                                                              \
    }                                                                          \
  } while (0)

static ryspec_status parse_string(const char *src) {
  ryspec_diagnostic diag;
  ryspec_document *doc = ryspec_parse(src, strlen(src), "<test>", &diag);
  CHECK((doc != NULL) == (diag.status == RYSPEC_OK));
  ryspec_document_free(doc);
  return diag.status;
}

static void test_version(void) {
  CHECK(strcmp(ryspec_version(), "0.1.0") == 0);
}

static void test_parse(void) {
  CHECK(parse_string("version = \"0\"\n") == RYSPEC_OK);
  CHECK(parse_string("version = \"0\"\n[properties.p]\ncheck = \"({p})\"\n") ==
        RYSPEC_OK);
  CHECK(parse_string("version = \n") == RYSPEC_ERROR_TOML);
  CHECK(parse_string("[properties.p]\ncheck = \"({p})\"\n") ==
        RYSPEC_ERROR_SCHEMA);
  CHECK(parse_string("version = 0\n") == RYSPEC_ERROR_SCHEMA);
  CHECK(parse_string("version = \"1\"\n") == RYSPEC_ERROR_SCHEMA);
}

static void test_parse_file(const char *root) {
  char path[4096];
  snprintf(path, sizeof path, "%s/examples/prefix_form.toml", root);
  ryspec_diagnostic diag;
  ryspec_document *doc = ryspec_parse_file(path, &diag);
  if (!doc) {
    fprintf(stderr, "%s:%d:%d: %s\n", path, diag.line, diag.column,
            diag.message);
  }
  CHECK(doc != NULL);
  if (doc) {
    CHECK(strcmp(ryspec_document_version(doc), "0") == 0);
  }
  ryspec_document_free(doc);

  CHECK(ryspec_parse_file("/nonexistent/ryspec.toml", &diag) == NULL);
  CHECK(diag.status == RYSPEC_ERROR_IO);
}

static void count_finding(const ryspec_diagnostic *diag, void *ctx) {
  (void)diag;
  ++*(int *)ctx;
}

static void test_lint(void) {
  const char *src = "version = \"0\"\n";
  ryspec_document *doc = ryspec_parse(src, strlen(src), "<test>", NULL);
  CHECK(doc != NULL);
  if (doc) {
    int reported = 0;
    CHECK(ryspec_lint(doc, count_finding, &reported) == 0);
    CHECK(reported == 0);
    CHECK(ryspec_lint(doc, NULL, NULL) == 0);
  }
  ryspec_document_free(doc);
}

int main(int argc, char **argv) {
  test_version();
  test_parse();
  test_lint();
  if (argc > 1) {
    test_parse_file(argv[1]);
  }
  if (failures) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  return 0;
}
