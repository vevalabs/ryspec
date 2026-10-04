/* Smoke tests for libryspec. The one argument is the repository root, so the
 * documented examples can be parsed from it. */
#include "ryspec/ryspec.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if(!(cond)) {                                                              \
      fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
      failures++;                                                              \
    }                                                                          \
  } while(0)

static int parse_string(const char* src)
{
  ryspec_diag diag;
  ryspec_toml_doc* doc = ryspec_toml_parse(src, strlen(src), &diag);
  CHECK((doc != NULL) == (diag.status == RYSPEC_OK));
  ryspec_toml_doc_free(doc);
  return diag.status;
}

/* Parse src and expect status, at line. */
static void expect_error(const char* src, int status, int line)
{
  ryspec_diag diag;
  ryspec_toml_doc* doc = ryspec_toml_parse(src, strlen(src), &diag);
  CHECK(doc == NULL);
  if(diag.status != status || diag.line != line) {
    fprintf(
      stderr,
      "%d:%d: status %d: %s\n",
      diag.line,
      diag.column,
      (int)diag.status,
      diag.message);
  }
  CHECK(diag.status == status);
  CHECK(diag.line == line);
  CHECK(strncmp(diag.message, "(line", 5) != 0);
  ryspec_toml_doc_free(doc);
}

static void test_toml_errors(void)
{
  expect_error("version = \"0\"\nx = \n", RYSPEC_ERROR_TOML, 2);
  expect_error("x = \"\\uD800\"\n", RYSPEC_ERROR_TOML_ENCODING, 1);
  expect_error("x = 1\nx = 2\n", RYSPEC_ERROR_TOML_REDEFINED, 2);
  expect_error("[a]\n[a]\n", RYSPEC_ERROR_TOML_REDEFINED, 2);
  expect_error("a = { b = 1 }\n[a.c]\n", RYSPEC_ERROR_TOML_REDEFINED, 2);

  char deep[64] = "\nx = ";
  memset(deep + 5, '[', 40);
  deep[45] = '\n';
  expect_error(deep, RYSPEC_ERROR_TOML_LIMIT, 2);
}

static void test_version(void)
{
  CHECK(ryspec_version() == 100);
}

static void test_parse(void)
{
  CHECK(parse_string("version = \"0\"\n") == RYSPEC_OK);
  CHECK(
    parse_string("version = \"0\"\n[properties.p]\ncheck = \"({p})\"\n") ==
    RYSPEC_OK);
  CHECK(parse_string("version = \n") == RYSPEC_ERROR_TOML);
  CHECK(
    parse_string("[properties.p]\ncheck = \"({p})\"\n") == RYSPEC_ERROR_SCHEMA);
  CHECK(parse_string("version = 0\n") == RYSPEC_ERROR_SCHEMA);
  CHECK(parse_string("version = \"1\"\n") == RYSPEC_ERROR_SCHEMA);
}

static void test_parse_file(const char* root)
{
  char path[4096];
  snprintf(path, sizeof path, "%s/examples/prefix_form.toml", root);
  ryspec_diag diag;
  ryspec_toml_doc* doc = ryspec_toml_parse_file(path, &diag);
  if(!doc) {
    fprintf(
      stderr, "%s:%d:%d: %s\n", path, diag.line, diag.column, diag.message);
  }
  CHECK(doc != NULL);
  ryspec_toml_doc_free(doc);

  CHECK(ryspec_toml_parse_file("/nonexistent/ryspec.toml", &diag) == NULL);
  CHECK(diag.status == RYSPEC_ERROR_IO);
}

int main(int argc, char** argv)
{
  test_version();
  test_parse();
  test_toml_errors();
  if(argc > 1) {
    test_parse_file(argv[1]);
  }
  if(failures) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  return 0;
}
