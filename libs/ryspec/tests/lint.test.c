/* Tests of the linter over the corpus. The one argument is the repository
 * root.
 *
 * Every check runs alone: the schema, as rule 0, and each rule of SPEC.md.
 * A file of examples/ or data/valid/ passes every check. A file of
 * data/semantic/ breaks the rule its `# Rule N` comment names and no other,
 * and so does a file of data/invalid/ declaring `#:expect-semantic-error`,
 * by the table below, its message holding the text it declares. A file
 * whose rule the linter does not check yet must pass every rule it does. A
 * file of data/invalid/ declaring `#:expect-schema-error` fails the schema
 * check, or is refused by the parser, which reads `version` and the
 * grammar of an expression before any check runs; its text is
 * jsonschema's, and not held to here. */
#define _POSIX_C_SOURCE 200809L

#include "lint.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ryspec/ryspec.h"

static int failures;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if(!(cond)) {                                                              \
      fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
      failures++;                                                              \
    }                                                                          \
  } while(0)

/* No rule broken. */
#define NONE (-1)

/* The rule of each file of data/invalid/ declaring a semantic error, as
 * SPEC.md's "What the schema cannot check" cites it. */
static const struct {
  const char* file;
  int rule;
} invalid_rules[] = {
  {"source_head_without_format.toml", 8},
  {"rule_shares_name_with_input.toml", 11},
  {"mixed_time_cones.toml", 12},
  {"cone_mixed_through_rule_name.toml", 12},
  {"operators/prev_mixed_cone.toml", 12},
  {"operators/once_mixed_cone.toml", 12},
  {"operators/historically_mixed_cone.toml", 12},
  {"operators/since_mixed_cone.toml", 12},
  {"operators/next_mixed_cone.toml", 12},
  {"operators/eventually_mixed_cone.toml", 12},
  {"operators/always_mixed_cone.toml", 12},
  {"operators/until_mixed_cone.toml", 12},
  {"rule_defined_in_terms_of_itself.toml", 13},
  {"assign_without_quantifier.toml", 14},
  {"quantifier_rebinds_enclosing_variable.toml", 15},
  {"quantifier_shadows_declared_name.toml", 16},
  {"quantifier_unused_variable.toml", 17},
  {"rule_bound_min_greater_than_max.toml", 7},
  {"operators/since_bound_min_greater_than_max.toml", 7},
  {"operators/until_bound_min_greater_than_max.toml", 7},
  {"parameter_with_source.toml", 18},
  {"bound_names_text_variable.toml", 19},
};

static bool checked(int rule)
{
  size_t n;
  const int* rules = ryspec_lint_rules(&n);
  for(size_t i = 0; i < n; i++) {
    if((int)rules[i] == rule) {
      return true;
    }
  }
  return false;
}

/* The file's header: the rule a `# Rule N` comment names, or 0, and the
 * text after `#:expect-semantic-error `, or "" when it declares none. */
typedef struct header {
  int rule;
  bool schema;
  bool semantic;
  char text[256];
} header;

static header read_header(const char* path)
{
  header h = {0};
  FILE* fp = fopen(path, "r");
  if(!fp) {
    return h;
  }
  static const char marker[] = "#:expect-semantic-error";
  char buf[1024];
  while(fgets(buf, sizeof buf, fp)) {
    buf[strcspn(buf, "\r\n")] = '\0';
    if(strncmp(buf, marker, sizeof marker - 1) == 0) {
      h.semantic = true;
      const char* text = buf + sizeof marker - 1;
      snprintf(h.text, sizeof h.text, "%s", text + strspn(text, " "));
    }
    if(strncmp(buf, "#:expect-schema-error", 21) == 0) {
      h.schema = true;
    }
    const char* rule = strstr(buf, "# Rule ");
    if(rule && !h.rule) {
      h.rule = atoi(rule + 7);
    }
  }
  fclose(fp);
  return h;
}

static ryspec_toml_doc* parse(const char* path)
{
  ryspec_diag diag;
  ryspec_toml_doc* doc = ryspec_toml_parse_file(path, &diag);
  if(!doc) {
    fprintf(
      stderr, "%s:%d:%d: %s\n", path, diag.line, diag.column, diag.message);
    failures++;
  }
  return doc;
}

static int status_of(int rule)
{
  return rule == RYSPEC_LINT_SCHEMA ? RYSPEC_ERROR_SCHEMA
                                    : RYSPEC_ERROR_SEMANTIC;
}

/* Lint the file at path rule by rule: the one rule broken, or NONE, fails
 * with a placed error holding text, and every other passes. */
static void expect_file(const char* path, int broken, const char* text)
{
  ryspec_toml_doc* doc = parse(path);
  if(!doc) {
    return;
  }
  size_t n;
  const int* rules = ryspec_lint_rules(&n);
  for(size_t i = 0; i < n; i++) {
    ryspec_diag diag;
    int s = ryspec_lint_rule(doc, rules[i], &diag);
    if((int)rules[i] != broken) {
      if(s != RYSPEC_OK) {
        fprintf(
          stderr,
          "%s:%d:%d: rule %d: %s\n",
          path,
          diag.line,
          diag.column,
          (int)rules[i],
          diag.message);
        failures++;
      }
      continue;
    }
    if(s != status_of(broken) || diag.status != s || diag.line == 0) {
      fprintf(stderr, "%s: rule %d not broken, or unplaced\n", path, broken);
      failures++;
    }
    else if(text && !strstr(diag.message, text)) {
      fprintf(
        stderr,
        "%s:%d:%d: rule %d: \"%s\" lacks \"%s\"\n",
        path,
        diag.line,
        diag.column,
        broken,
        diag.message,
        text);
      failures++;
    }
  }
  /* All the rules at once: the first violation, or none. */
  ryspec_diag diag;
  int s = ryspec_toml_lint(doc, &diag);
  CHECK(s == (checked(broken) ? status_of(broken) : RYSPEC_OK));
  CHECK(diag.status == s);
  ryspec_toml_doc_free(doc);
}

/* Each *.toml under dir, by its path and its path relative to root. */
typedef void (*file_fn)(const char* path, const char* relative);

static int each_file(const char* dir, size_t root_len, file_fn fn)
{
  DIR* d = opendir(dir);
  if(!d) {
    perror(dir);
    failures++;
    return 0;
  }
  int files = 0;
  struct dirent* e;
  while((e = readdir(d))) {
    if(e->d_name[0] == '.') {
      continue;
    }
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
    struct stat st;
    if(stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
      files += each_file(path, root_len, fn);
      continue;
    }
    size_t n = strlen(e->d_name);
    if(n > 5 && strcmp(e->d_name + n - 5, ".toml") == 0) {
      fn(path, path + root_len + 1);
      files++;
    }
  }
  closedir(d);
  return files;
}

static int under(const char* root, const char* dir, file_fn fn)
{
  char path[4096];
  snprintf(path, sizeof path, "%s/%s", root, dir);
  return each_file(path, strlen(path), fn);
}

static void clean(const char* path, const char* relative)
{
  (void)relative;
  expect_file(path, NONE, NULL);
}

/* A file the schema rejects: refused at parse, or failing the schema check
 * alone and so ryspec_toml_lint(). */
static int refused_at_parse;

static void expect_schema_error(const char* path)
{
  ryspec_diag diag;
  ryspec_toml_doc* doc = ryspec_toml_parse_file(path, &diag);
  if(!doc) {
    if(
      diag.status != RYSPEC_ERROR_SCHEMA &&
      diag.status != RYSPEC_ERROR_GRAMMAR) {
      fprintf(
        stderr,
        "%s:%d:%d: refused, not by the schema: %s\n",
        path,
        diag.line,
        diag.column,
        diag.message);
      failures++;
    }
    refused_at_parse++;
    return;
  }
  int s = ryspec_lint_rule(doc, RYSPEC_LINT_SCHEMA, &diag);
  if(s != RYSPEC_ERROR_SCHEMA) {
    fprintf(stderr, "%s: passes the schema check\n", path);
    failures++;
  }
  else {
    CHECK(ryspec_toml_lint(doc, &diag) == RYSPEC_ERROR_SCHEMA);
  }
  ryspec_toml_doc_free(doc);
}

static void semantic(const char* path, const char* relative)
{
  (void)relative;
  header h = read_header(path);
  if(h.rule == 0) {
    fprintf(stderr, "%s: no `# Rule N` comment\n", path);
    failures++;
    return;
  }
  expect_file(path, h.rule, NULL);
}

static void invalid(const char* path, const char* relative)
{
  header h = read_header(path);
  if(h.schema) {
    expect_schema_error(path);
    return;
  }
  if(!h.semantic) {
    return;
  }
  for(size_t i = 0; i < sizeof invalid_rules / sizeof invalid_rules[0]; i++) {
    if(strcmp(invalid_rules[i].file, relative) == 0) {
      expect_file(path, invalid_rules[i].rule, h.text);
      return;
    }
  }
  fprintf(stderr, "%s: declares a semantic error and has no rule\n", path);
  failures++;
}

/* ---------------------------------------------------------------------------
 * Documents written here. */

static int lint_string(const char* src, ryspec_diag* diag)
{
  ryspec_toml_doc* doc = ryspec_toml_parse(src, strlen(src), NULL);
  CHECK(doc != NULL);
  int s = ryspec_toml_lint(doc, diag);
  ryspec_toml_doc_free(doc);
  return s;
}

static void test_lint(void)
{
  ryspec_diag diag = {.status = RYSPEC_ERROR_SCHEMA};
  CHECK(
    lint_string("version = \"0\"\n[rules]\nr = [\"once\", \"p\"]\n", &diag) ==
    RYSPEC_OK);
  CHECK(diag.status == RYSPEC_OK);
  CHECK(lint_string("version = \"0\"\n", NULL) == RYSPEC_OK);

  /* The first violation, placed. */
  CHECK(
    lint_string(
      "version = \"0\"\n"
      "[variables]\n"
      "x = { type = \"number\", min = 3, max = 1 }\n",
      &diag) == RYSPEC_ERROR_SEMANTIC);
  CHECK(diag.line == 3);
  CHECK(strstr(diag.message, "min (3) is greater than max (1)") != NULL);

  /* Expression form: the bound is placed in its string. */
  CHECK(
    lint_string(
      "version = \"0\"\n"
      "[rules]\n"
      "r = \"(once[5:2] {p})\"\n",
      &diag) == RYSPEC_ERROR_SEMANTIC);
  CHECK(diag.line == 3 && diag.column > 0);

  /* A dotted path resolves from the root, and one nothing answers fails. */
  CHECK(
    lint_string(
      "version = \"0\"\n"
      "[namespace.a.b.rules]\n"
      "r = \"p\"\n"
      "[properties.q]\n"
      "check = \"a.b.r\"\n",
      &diag) == RYSPEC_OK);
  CHECK(
    lint_string(
      "version = \"0\"\n"
      "[namespace.a.b.rules]\n"
      "r = \"p\"\n"
      "[properties.q]\n"
      "check = \"a.r\"\n",
      &diag) == RYSPEC_ERROR_SEMANTIC);
  CHECK(diag.line == 5 && strstr(diag.message, "resolves to no rule"));

  /* Text read as a number is Rule 9's; a bool compared as text Rule 20's. */
  CHECK(
    lint_string(
      "version = \"0\"\n"
      "[variables]\n"
      "t = { type = \"text\" }\n"
      "[properties.q]\n"
      "check = [\"gt\", \"t\", { value = 1 }]\n",
      &diag) == RYSPEC_ERROR_SEMANTIC);
  CHECK(strstr(diag.message, "reads it as a number") != NULL);
  CHECK(
    lint_string(
      "version = \"0\"\n"
      "[variables]\n"
      "b = { type = \"bool\" }\n"
      "[properties.q]\n"
      "check = [\"contains\", \"b\", { value = \"x\" }]\n",
      &diag) == RYSPEC_ERROR_SEMANTIC);
  CHECK(strstr(diag.message, "compares text") != NULL);

  /* A property named for its verdict inside its own check is a ring. */
  CHECK(
    lint_string(
      "version = \"0\"\n"
      "[properties.q]\n"
      "check = [\"once\", \"q\"]\n",
      &diag) == RYSPEC_ERROR_SEMANTIC);
  CHECK(strstr(diag.message, "`q` is defined in terms of itself") != NULL);

  /* Cones mix in expression form as in prefix form; a neutral rule serves
   * both. */
  CHECK(
    lint_string(
      "version = \"0\"\n"
      "[rules]\n"
      "r = \"(once {p} and eventually {p})\"\n",
      &diag) == RYSPEC_ERROR_SEMANTIC);
  CHECK(diag.line == 3 && strstr(diag.message, "past-time rule"));
  CHECK(
    lint_string(
      "version = \"0\"\n"
      "[rules]\n"
      "n = [\"not\", \"p\"]\n"
      "a = [\"once\", \"n\"]\n"
      "b = [\"eventually\", \"n\"]\n",
      &diag) == RYSPEC_OK);

  /* Quantifiers in expression form: nested and used, or bound and unused. */
  CHECK(
    lint_string(
      "version = \"0\"\n"
      "[variables]\n"
      "x = { type = \"number\" }\n"
      "y = { type = \"number\" }\n"
      "[rules]\n"
      "r = \"(forall[a] exists[b] ({a := x} and {b := y}))\"\n",
      &diag) == RYSPEC_OK);
  CHECK(
    lint_string(
      "version = \"0\"\n"
      "[variables]\n"
      "x = { type = \"number\" }\n"
      "[rules]\n"
      "r = \"(forall[s] {x < 120})\"\n",
      &diag) == RYSPEC_ERROR_SEMANTIC);
  CHECK(diag.line == 5 && strstr(diag.message, "never used"));

  /* The schema first: an expression is translated only at a rule position,
   * and one inside a prefix rule is no dotted path. */
  CHECK(
    lint_string(
      "version = \"0\"\n"
      "[rules]\n"
      "r = [\"not\", \"({p})\"]\n",
      &diag) == RYSPEC_ERROR_SCHEMA);
  CHECK(diag.line == 3 && strstr(diag.message, "`rules.r[1]`") != NULL);
  CHECK(
    lint_string(
      "version = \"0\"\n"
      "[variables]\n"
      "x = { type = \"number\", min = 3, max = 1, unit = \"s\" }\n",
      &diag) == RYSPEC_ERROR_SCHEMA);
  CHECK(strstr(diag.message, "takes no key `unit`") != NULL);

  /* Nothing under extras is checked. */
  CHECK(
    lint_string(
      "version = \"0\"\n"
      "[extras]\n"
      "x = nan\n"
      "b = { min = 3, max = 1 }\n"
      "[variables.v]\n"
      "extras = { initial_value = nan }\n",
      &diag) == RYSPEC_OK);
}

static void test_lint_rule(void)
{
  const char* src = "version = \"0\"\n";
  ryspec_toml_doc* doc = ryspec_toml_parse(src, strlen(src), NULL);
  ryspec_diag diag;
  CHECK(ryspec_lint_rule(doc, RYSPEC_LINT_NO_NAN, &diag) == RYSPEC_OK);
  CHECK(ryspec_lint_rule(doc, 2, &diag) == RYSPEC_ERROR_SEMANTIC);
  CHECK(strcmp(diag.message, "no lint rule 2") == 0 && diag.line == 0);
  CHECK(ryspec_lint_rule(doc, 2, NULL) == RYSPEC_ERROR_SEMANTIC);
  ryspec_toml_doc_free(doc);

  size_t n;
  const int* rules = ryspec_lint_rules(&n);
  CHECK(n > 0);
  for(size_t i = 1; i < n; i++) {
    CHECK(rules[i - 1] < rules[i]);
  }
}

int main(int argc, char** argv)
{
  if(argc != 2) {
    fprintf(stderr, "usage: %s REPOSITORY_ROOT\n", argv[0]);
    return 2;
  }
  test_lint();
  test_lint_rule();
  CHECK(under(argv[1], "examples", clean) > 0);
  CHECK(under(argv[1], "data/valid", clean) > 0);
  CHECK(under(argv[1], "data/semantic", semantic) > 0);
  CHECK(under(argv[1], "data/invalid", invalid) > 0);
  /* missing_version, version_not_*, empty_expression and
   * expression_trailing_whitespace */
  CHECK(refused_at_parse <= 5);
  if(failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  return 0;
}
