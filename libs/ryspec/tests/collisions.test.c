/* Tests of Rule 11, one source of value per visible name, by case: which
 * names collide, which do not, and, where a document holds several
 * collisions, which one the check reports. The one argument, the repository
 * root, is not read.
 *
 * Each case pins the status, the message and the place. The order the check
 * reports in is its own: variables, then rule positions, then properties,
 * then top-level namespaces, each in document order, the first name to meet
 * an earlier one reported. The two "order" cases each hold a collision the
 * document's own order would find first, and expect the other. Monitors
 * are no source of value: a monitor shares its name with anything. */
#include <stdio.h>
#include <string.h>

#include "ryspec/ryspec.h"

#include "lint.h"

static int failures;

#define V "version = \"0\"\n"

static const struct {
  const char* name;
  const char* src;
  int status;
  int line, column;
  const char* message;
} cases[] = {
  {"variable vs rule",
   V "[variables]\n"
     "x = { type = \"bool\" }\n"
     "[rules]\n"
     "x = \"y\"\n",
   RYSPEC_ERROR_SEMANTIC,
   5,
   6,
   "`x` has more than one source of value: a variable and a rule"},
  {"variable vs property",
   V "[variables]\n"
     "x = { type = \"bool\" }\n"
     "[properties.x]\n"
     "check = \"y\"\n",
   RYSPEC_ERROR_SEMANTIC,
   4,
   2,
   "`x` has more than one source of value: a variable and a property"},
  {"variable vs private rule",
   V "[variables]\n"
     "x = { type = \"bool\" }\n"
     "[properties.p]\n"
     "check = \"x\"\n"
     "[properties.p.where]\n"
     "x = \"y\"\n",
   RYSPEC_ERROR_SEMANTIC,
   7,
   6,
   "`x` has more than one source of value: a variable and a private rule"},
  {"variable vs top-level namespace",
   V "[variables]\n"
     "x = { type = \"bool\" }\n"
     "[namespace.x.rules]\n"
     "r = \"y\"\n",
   RYSPEC_ERROR_SEMANTIC,
   4,
   2,
   "`x` has more than one source of value: a variable and a namespace"},
  {"rule vs property",
   V "[rules]\n"
     "a = \"y\"\n"
     "[properties.a]\n"
     "check = \"y\"\n",
   RYSPEC_ERROR_SEMANTIC,
   4,
   2,
   "`a` has more than one source of value: a rule and a property"},
  {"private rule vs namespace rule",
   V "[rules]\n"
     "a = \"y\"\n"
     "[properties.p]\n"
     "check = \"a\"\n"
     "[properties.p.where]\n"
     "a = \"z\"\n",
   RYSPEC_ERROR_SEMANTIC,
   7,
   6,
   "`a` has more than one source of value: a rule and a private rule"},
  {"private rule vs property",
   V "[properties.a]\n"
     "check = \"y\"\n"
     "[properties.p]\n"
     "check = \"z\"\n"
     "[properties.p.where]\n"
     "a = \"w\"\n",
   RYSPEC_ERROR_SEMANTIC,
   7,
   6,
   "`a` has more than one source of value: a property and a private rule"},
  {"private rule vs property, named namespace",
   V "[namespace.n.properties.a]\n"
     "check = \"y\"\n"
     "[namespace.n.properties.p]\n"
     "check = \"z\"\n"
     "[namespace.n.properties.p.where]\n"
     "a = \"w\"\n",
   RYSPEC_ERROR_SEMANTIC,
   7,
   6,
   "`a` has more than one source of value: a property and a private rule"},
  {"same private name in two properties",
   V "[properties.p]\n"
     "check = \"a\"\n"
     "[properties.p.where]\n"
     "a = \"y\"\n"
     "[properties.q]\n"
     "check = \"a\"\n"
     "[properties.q.where]\n"
     "a = \"z\"\n",
   RYSPEC_OK,
   0,
   0,
   ""},
  {"same name in unrelated namespaces",
   V "[namespace.n.rules]\n"
     "a = \"y\"\n"
     "[namespace.m.rules]\n"
     "a = \"z\"\n",
   RYSPEC_OK,
   0,
   0,
   ""},
  {"same name in parent and child namespaces",
   V "[namespace.n.rules]\n"
     "a = \"y\"\n"
     "[namespace.n.m.rules]\n"
     "a = \"z\"\n",
   RYSPEC_OK,
   0,
   0,
   ""},
  {"root rule vs named namespace rule",
   V "[rules]\n"
     "a = \"y\"\n"
     "[namespace.n.rules]\n"
     "a = \"z\"\n",
   RYSPEC_OK,
   0,
   0,
   ""},
  {"top-level namespace vs root rule",
   V "[rules]\n"
     "n = \"y\"\n"
     "[namespace.n.rules]\n"
     "r = \"z\"\n",
   RYSPEC_ERROR_SEMANTIC,
   4,
   2,
   "`n` has more than one source of value: a rule and a namespace"},
  {"top-level namespace vs root property",
   V "[properties.n]\n"
     "check = \"y\"\n"
     "[namespace.n.rules]\n"
     "r = \"z\"\n",
   RYSPEC_ERROR_SEMANTIC,
   4,
   2,
   "`n` has more than one source of value: a property and a namespace"},
  {"nested namespace vs root rule",
   V "[rules]\n"
     "m = \"y\"\n"
     "[namespace.n.m.rules]\n"
     "r = \"z\"\n",
   RYSPEC_OK,
   0,
   0,
   ""},
  {"nested namespace vs its parent's rule",
   V "[namespace.n.rules]\n"
     "m = \"y\"\n"
     "[namespace.n.m.rules]\n"
     "r = \"z\"\n",
   RYSPEC_OK,
   0,
   0,
   ""},
  {"monitor vs variable",
   V "[monitors.x]\n"
     "inputs = [\"x\"]\n"
     "[variables]\n"
     "x = { type = \"bool\" }\n",
   RYSPEC_OK,
   0,
   0,
   ""},
  {"monitor vs rule",
   V "[monitors.a]\n"
     "outputs = [\"a\"]\n"
     "[rules]\n"
     "a = \"y\"\n",
   RYSPEC_OK,
   0,
   0,
   ""},
  {"monitor vs property",
   V "[monitors.a]\n"
     "outputs = [\"a\"]\n"
     "[properties.a]\n"
     "check = \"y\"\n",
   RYSPEC_OK,
   0,
   0,
   ""},
  {"monitor vs top-level namespace",
   V "[monitors.n]\n"
     "inputs = [\"y\"]\n"
     "[namespace.n.rules]\n"
     "r = \"y\"\n",
   RYSPEC_OK,
   0,
   0,
   ""},
  {"legacy order: positions before properties",
   V "[rules]\n"
     "a = \"y\"\n"
     "[properties.a]\n"
     "check = \"y\"\n"
     "[namespace.n.rules]\n"
     "b = \"y\"\n"
     "[namespace.n.properties.q]\n"
     "check = \"b\"\n"
     "[namespace.n.properties.q.where]\n"
     "b = \"z\"\n",
   RYSPEC_ERROR_SEMANTIC,
   11,
   6,
   "`b` has more than one source of value: a rule and a private rule"},
  {"legacy order: namespaces last",
   V "[rules]\n"
     "n = \"y\"\n"
     "[namespace.n.rules]\n"
     "b = \"y\"\n"
     "[namespace.n.properties.q]\n"
     "check = \"b\"\n"
     "[namespace.n.properties.q.where]\n"
     "b = \"z\"\n",
   RYSPEC_ERROR_SEMANTIC,
   9,
   6,
   "`b` has more than one source of value: a rule and a private rule"},
};

int main(void)
{
  for(size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    ryspec_diag diag;
    ryspec_toml_doc* doc =
      ryspec_toml_parse(cases[i].src, strlen(cases[i].src), &diag);
    if(!doc) {
      fprintf(stderr, "%s: does not parse: %s\n", cases[i].name, diag.message);
      failures++;
      continue;
    }
    int s = ryspec_lint_rule(doc, RYSPEC_LINT_ONE_SOURCE_OF_VALUE, &diag);
    if(
      s != cases[i].status || diag.line != cases[i].line ||
      diag.column != cases[i].column ||
      strcmp(diag.message, cases[i].message) != 0) {
      fprintf(
        stderr,
        "%s: got %d at %d:%d \"%s\", want %d at %d:%d \"%s\"\n",
        cases[i].name,
        (int)s,
        diag.line,
        diag.column,
        diag.message,
        (int)cases[i].status,
        cases[i].line,
        cases[i].column,
        cases[i].message);
      failures++;
    }
    ryspec_toml_doc_free(doc);
  }
  if(failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  return 0;
}
