/* Tests of the expression parser over the corpus. The one argument is the
 * repository root: a file of data/malformed/ must fail to parse with a
 * grammar error on the line of its expression, and a file of examples/,
 * data/valid/ or data/semantic/ must parse, its every expression
 * translated. */
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ryspec/ryspec.h"

static int failures;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if(!(cond)) {                                                              \
      fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
      failures++;                                                              \
    }                                                                          \
  } while(0)

/* The line of the first expression-form string in the file at path, or 0. */
static int expression_line(const char* path)
{
  FILE* fp = fopen(path, "r");
  if(!fp) {
    return 0;
  }
  char buf[1024];
  int line = 0, found = 0;
  while(!found && fgets(buf, sizeof buf, fp)) {
    line++;
    if(buf[0] != '#' && strstr(buf, "\"(")) {
      found = line;
    }
  }
  fclose(fp);
  return found;
}

/* Parse the file at path, expecting a grammar error, or none. */
static void expect_file(const char* path, bool error)
{
  ryspec_diag diag;
  ryspec_toml_doc* doc = ryspec_toml_parse_file(path, &diag);
  ryspec_toml_doc_free(doc);
  if(!error) {
    if(!doc) {
      fprintf(
        stderr, "%s:%d:%d: %s\n", path, diag.line, diag.column, diag.message);
      failures++;
    }
    return;
  }
  if(doc) {
    fprintf(stderr, "%s: parsed\n", path);
    failures++;
    return;
  }
  CHECK(diag.status == RYSPEC_ERROR_GRAMMAR);
  int line = expression_line(path);
  if(diag.line != line) {
    fprintf(
      stderr,
      "%s:%d:%d: error not on line %d: %s\n",
      path,
      diag.line,
      diag.column,
      line,
      diag.message);
    failures++;
  }
}

static int expect_dir(const char* root, const char* dir, bool error)
{
  char path[4096];
  snprintf(path, sizeof path, "%s/%s", root, dir);
  DIR* d = opendir(path);
  if(!d) {
    perror(path);
    failures++;
    return 0;
  }
  int files = 0;
  struct dirent* e;
  while((e = readdir(d))) {
    size_t n = strlen(e->d_name);
    if(n > 5 && strcmp(e->d_name + n - 5, ".toml") == 0) {
      char file[4096 + 256];
      snprintf(file, sizeof file, "%s/%s", path, e->d_name);
      expect_file(file, error);
      files++;
    }
  }
  closedir(d);
  return files;
}

int main(int argc, char** argv)
{
  if(argc != 2) {
    fprintf(stderr, "usage: %s REPOSITORY_ROOT\n", argv[0]);
    return 2;
  }
  CHECK(expect_dir(argv[1], "data/malformed", true) >= 20);
  CHECK(expect_dir(argv[1], "examples", false) > 0);
  CHECK(expect_dir(argv[1], "data/valid", false) > 0);
  CHECK(expect_dir(argv[1], "data/semantic", false) > 0);
  if(failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  return 0;
}
