// ryspec-db -- build the in-memory database over documents and report on it.
//
// `ryspec-parse` answers the first of the three questions a ryspec document
// has to pass: is this well formed? This answers the third, the loader's --
// what its names mean and whether they hold together -- and dumps the store it
// built on the way, which is the only view of the deduplication there is.
//
//   ryspec-db data/valid                    every file must check out
//   ryspec-db --expect-invalid data/invalid every file must report
//   ryspec-db --corpus data                 each file's header decides
//   ryspec-db --together --stats data/valid what a whole corpus shares
//
// A file at a time by default, each in a database of its own, because that is
// what a document means on its own terms. --together puts every file named
// into ONE database instead, which is what the namespace is for: names from
// two files meet under theirs, a rule they both state is stored once, and two
// files sharing a namespace are checked against each other. Pointed at a
// corpus of documents that declare no namespace, --together says they are all
// one document -- which they are, by the format's rules, and the diagnostics
// it then reports are real.

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "database.h"

#define PROGRAM "ryspec-db"

#ifndef RYSPEC_VERSION
#define RYSPEC_VERSION "unknown"
#endif

static const char USAGE[] =
    "usage: " PROGRAM " [options] <path>...\n"
    "\n"
    "Load each *.toml file into a database of its own, check it, and report.\n"
    "\n"
    "options:\n"
    "  --dump-symbols   the symbol table, by qualified name\n"
    "  --dump-rules     the rule table, one interned term per line\n"
    "  --dump-strings   the intern table, with each entry's use count\n"
    "  --stats          how much of the corpus is shared rather than repeated\n"
    "  --together       load every file into ONE database, under its namespace\n"
    "  --corpus         let each file's #:expect-semantic-error header decide\n"
    "  --expect-invalid assert every file reports at least one diagnostic\n"
    "  --quiet          report failures only, not the closing summary\n"
    "  --version        print the version and exit\n"
    "  --help           print this message and exit\n";

// What every file owes, unless `corpus` takes it from the file's own header.
typedef enum { OWES_CLEAN, OWES_DIAGNOSTIC } expectation;

typedef struct {
  bool dump_symbols;
  bool dump_rules;
  bool dump_strings;
  bool stats;
  bool together;
  bool corpus;
  expectation expectation;
  bool quiet;
} options;

// ------------------------------------------------------------------ dumps

static const struct {
  uint32_t role;
  const char *name;
} ROLES[] = {{RYSPEC_ROLE_VARIABLE, "variable"}, {RYSPEC_ROLE_INPUT, "input"},
             {RYSPEC_ROLE_OUTPUT, "output"},     {RYSPEC_ROLE_PARAMETER, "parameter"},
             {RYSPEC_ROLE_RULE, "rule"},         {RYSPEC_ROLE_PROPERTY, "property"},
             {RYSPEC_ROLE_IMPLICIT_INPUT, "implicit-input"}};

static const char *TYPES[] = {"number", "bool", "text", "binary"};
static const char *CRITICALITIES[] = {NULL, "info", "warning", "error", "critical"};

static void print_value(const char *label, ryspec_value value) {
  switch (value.kind) {
    case RYSPEC_VALUE_ABSENT: return;
    case RYSPEC_VALUE_INTEGER: printf("  %s=%lld", label, (long long)value.integer); return;
    case RYSPEC_VALUE_FLOAT: printf("  %s=%g", label, value.real); return;
    case RYSPEC_VALUE_BOOLEAN: printf("  %s=%s", label, value.boolean ? "true" : "false"); return;
  }
}

static void print_symbols(const ryspec_database *db) {
  // A document at a time, so a namespace is read once rather than repeated on
  // every line it prefixes.
  for (uint32_t doc = 1; doc < ryspec_document_count(db); doc++) {
    const ryspec_document *document = ryspec_document_at(db, doc);
    printf("%s", ryspec_text(db, document->path, NULL));
    if (document->has_version) printf("  version=%lld", (long long)document->version);
    if (document->namespace_name != RYSPEC_NONE) {
      printf("  namespace=%s", ryspec_text(db, document->namespace_name, NULL));
    }
    printf("\n");
  }

  printf("symbols (%u)\n", ryspec_symbol_count(db) - 1);
  for (uint32_t id = 1; id < ryspec_symbol_count(db); id++) {
    const ryspec_symbol *symbol = ryspec_symbol_at(db, id);
    printf("  %-44s", ryspec_text(db, symbol->qualified, NULL));
    const char *separator = " ";
    for (size_t i = 0; i < sizeof ROLES / sizeof *ROLES; i++) {
      if (symbol->roles & ROLES[i].role) {
        printf("%s%s", separator, ROLES[i].name);
        separator = "+";
      }
    }
    // What the declaration adds: the interface detail a [variables] entry
    // carries, which is the whole reason for declaring a name that a rule
    // would otherwise deduce.
    if (symbol->has_type) printf("  type=%s", TYPES[symbol->type]);
    if (symbol->unit != RYSPEC_NONE) printf("  unit=%s", ryspec_text(db, symbol->unit, NULL));
    if (symbol->source != RYSPEC_NONE) printf("  source=%s", ryspec_text(db, symbol->source, NULL));
    if (symbol->format != RYSPEC_NONE) printf("  format=%s", ryspec_text(db, symbol->format, NULL));
    print_value("initial", symbol->initial_value);
    print_value("min", symbol->min);
    print_value("max", symbol->max);
    if (symbol->partition_order != 0) printf("  slot=%u", symbol->partition_order - 1);
    if (symbol->criticality != RYSPEC_CRITICALITY_NONE) {
      printf("  criticality=%s", CRITICALITIES[symbol->criticality]);
    }

    if (symbol->rule != RYSPEC_NONE) printf("  rule=r%u", symbol->rule);
    if (symbol->check != RYSPEC_NONE) printf("  check=r%u", symbol->check);
    if (symbol->given != RYSPEC_NONE) printf("  given=r%u", symbol->given);
    if (symbol->impose != RYSPEC_NONE) printf("  impose=r%u", symbol->impose);
    if (symbol->title != RYSPEC_NONE) printf("  title=%s", ryspec_text(db, symbol->title, NULL));
    if (symbol->description != RYSPEC_NONE) {
      printf("  description=%s", ryspec_text(db, symbol->description, NULL));
    }
    printf("\n");
  }
}

static void print_rules(const ryspec_database *db) {
  printf("rules (%u)\n", ryspec_rule_count(db) - 1);
  char buffer[1024];
  for (uint32_t id = 1; id < ryspec_rule_count(db); id++) {
    ryspec_rule_format(db, id, buffer, sizeof buffer);
    printf("  r%-5u %s\n", id, buffer);
  }
}

static void print_strings(const ryspec_database *db) {
  printf("strings (%u)\n", ryspec_string_count(db) - 1);
  for (uint32_t id = 1; id < ryspec_string_count(db); id++) {
    printf("  %6u  %s\n", ryspec_string_uses(db, id), ryspec_text(db, id, NULL));
  }
}

// Every rule the documents state: a rule's body, and a property's given, check
// and impose. The same set semantics.c walks, which is why the database keeps
// no separate record of it -- the symbol table already is one.
static uint32_t rule_positions(const ryspec_database *db) {
  uint32_t positions = 0;
  for (uint32_t id = 1; id < ryspec_symbol_count(db); id++) {
    const ryspec_symbol *symbol = ryspec_symbol_at(db, id);
    if (symbol->rule != RYSPEC_NONE) positions++;
    if (symbol->given != RYSPEC_NONE) positions++;
    if (symbol->check != RYSPEC_NONE) positions++;
    if (symbol->impose != RYSPEC_NONE) positions++;
  }
  return positions;
}

static void print_stats(const ryspec_database *db) {
  uint32_t strings = ryspec_string_count(db) - 1;
  uint32_t rules = ryspec_rule_count(db) - 1;
  uint32_t positions = rule_positions(db);

  uint32_t uses = 0;
  for (uint32_t id = 1; id < ryspec_string_count(db); id++) uses += ryspec_string_uses(db, id);

  // What a rule is worth as a shared thing: how many times a term is reachable
  // from the rules the documents state, against how many terms there are.
  uint32_t references = 0;
  for (uint32_t id = 1; id < ryspec_rule_count(db); id++) {
    uint32_t count = 0;
    ryspec_rule_operands(db, id, &count);
    references += count;
  }
  references += positions;

  printf("documents  %u\n", ryspec_document_count(db) - 1);
  printf("strings    %u unique of %u interned\n", strings, uses);
  printf("rules      %u unique of %u referenced\n", rules, references);
  printf("symbols    %u\n", ryspec_symbol_count(db) - 1);
  printf("rule positions %u, quantifier bindings %u\n", positions, ryspec_binding_count(db));
}

// ------------------------------------------------------------ expectations

// The `#:expect-semantic-error` header of data/README.md. The rest of the line
// is a substring of the reported message.
static bool declared_expectation(const char *source, size_t length, char *substring, size_t size) {
  static const char MARKER[] = "#:expect-semantic-error";
  size_t marker_length = sizeof MARKER - 1;

  for (size_t start = 0; start < length;) {
    size_t end = start;
    while (end < length && source[end] != '\n') end++;
    const char *line = source + start;
    size_t line_length = end - start;
    start = end + 1;

    if (line_length < marker_length || memcmp(line, MARKER, marker_length) != 0) continue;
    const char *rest = line + marker_length;
    size_t rest_length = line_length - marker_length;
    while (rest_length > 0 && (*rest == ' ' || *rest == '\t')) {
      rest++;
      rest_length--;
    }
    while (rest_length > 0 && (rest[rest_length - 1] == '\r' || rest[rest_length - 1] == ' ')) {
      rest_length--;
    }
    if (rest_length >= size) rest_length = size - 1;
    memcpy(substring, rest, rest_length);
    substring[rest_length] = '\0';
    return true;
  }
  return false;
}

static bool reports_substring(const ryspec_database *db, const char *substring) {
  char report[768];
  for (uint32_t i = 0; i < ryspec_diagnostic_count(db); i++) {
    ryspec_diagnostic_format(db, i, report, sizeof report);
    if (strstr(report, substring) != NULL) return true;
  }
  return false;
}

static void print_diagnostics(const ryspec_database *db) {
  char report[768];
  for (uint32_t i = 0; i < ryspec_diagnostic_count(db); i++) {
    ryspec_diagnostic_format(db, i, report, sizeof report);
    fprintf(stderr, "FAIL %s\n", report);
  }
}

// ---------------------------------------------------------------- the runs

static void dump(const ryspec_database *db, const options *options) {
  if (options->dump_symbols) print_symbols(db);
  if (options->dump_rules) print_rules(db);
  if (options->dump_strings) print_strings(db);
  if (options->stats) print_stats(db);
}

typedef struct {
  const options *options;
  uint32_t files;
  uint32_t failures;
} run;

// One file, in a database of its own. A document means what it means on its
// own terms, and two files that both declare `p` with no namespace are two
// documents rather than one collision.
static bool visit(ryspec_database *db, const char *file, void *context) {
  run *state = context;
  state->files++;
  dump(db, state->options);

  expectation owes = state->options->expectation;
  char substring[256] = {0};
  bool has_substring = false;

  if (state->options->corpus) {
    // The header lives in the file and the database does not keep the source,
    // so the expectation is read back off disk here.
    owes = OWES_CLEAN;
    FILE *handle = fopen(file, "rb");
    if (handle != NULL) {
      char header[4096];
      size_t length = fread(header, 1, sizeof header, handle);
      fclose(handle);
      if (declared_expectation(header, length, substring, sizeof substring)) {
        owes = OWES_DIAGNOSTIC;
        has_substring = substring[0] != '\0';
      }
    }
    // A file that does not parse is the grammar's fixture, not the database's:
    // a schema or grammar negative owes this layer nothing either way.
    const ryspec_document *document = ryspec_document_at(db, 1);
    if (document == NULL || !document->parsed) return true;
    if (owes == OWES_CLEAN && ryspec_diagnostic_count(db) > 0) return true;
  }

  uint32_t diagnostics = ryspec_diagnostic_count(db);
  if (owes == OWES_CLEAN) {
    if (diagnostics == 0) return true;
    print_diagnostics(db);
    state->failures++;
    return false;
  }

  if (diagnostics == 0) {
    fprintf(stderr, "FAIL %s: checks out, so it documents no loader error\n", file);
    state->failures++;
    return false;
  }
  if (has_substring && !reports_substring(db, substring)) {
    fprintf(stderr, "FAIL %s: reports, but not \"%s\"\n", file, substring);
    print_diagnostics(db);
    state->failures++;
    return false;
  }
  return true;
}

static int run_per_file(int argc, char **argv, int first, const options *options) {
  run state = {.options = options};
  bool ok = true;
  for (int i = first; i < argc; i++) {
    if (!ryspec_each_document(argv[i], visit, &state)) ok = false;
  }

  if (state.failures > 0) {
    fprintf(stderr, "\n%u of %u file(s) failed\n", state.failures, state.files);
    return 1;
  }
  if (!ok) return 2;
  if (!options->quiet) {
    const char *verdict = options->corpus              ? "matched their expectation"
                          : options->expectation == OWES_DIAGNOSTIC ? "reported"
                                                                    : "checked out";
    printf("ok: %u file(s) %s\n", state.files, verdict);
  }
  return 0;
}

// Everything named in one database, which is the mode that says anything about
// sharing across files -- and the only one in which a namespace does any work.
static int run_together(int argc, char **argv, int first, const options *options) {
  ryspec_database *db = ryspec_database_new();
  if (db == NULL) return 2;

  bool loaded = true;
  for (int i = first; i < argc; i++) {
    if (!ryspec_database_load(db, argv[i])) loaded = false;
  }
  ryspec_database_check(db);
  dump(db, options);

  uint32_t diagnostics = ryspec_diagnostic_count(db);
  int status = 0;
  if (options->expectation == OWES_DIAGNOSTIC) {
    if (diagnostics == 0) {
      fprintf(stderr, "FAIL the corpus checks out, so it documents no loader error\n");
      status = 1;
    } else if (!options->quiet) {
      printf("ok: %u diagnostic(s), as expected\n", diagnostics);
    }
  } else if (diagnostics > 0) {
    print_diagnostics(db);
    status = 1;
  } else if (!options->quiet) {
    printf("ok: %u document(s), %u symbol(s), %u unique rule(s)\n", ryspec_document_count(db) - 1,
           ryspec_symbol_count(db) - 1, ryspec_rule_count(db) - 1);
  }
  if (!loaded) status = 2;

  ryspec_database_free(db);
  return status;
}

// ------------------------------------------------------------------- main

int main(int argc, char **argv) {
  options options = {0};
  int first_path = argc;

  for (int i = 1; i < argc; i++) {
    const char *argument = argv[i];
    if (strcmp(argument, "--help") == 0) {
      fputs(USAGE, stdout);
      return 0;
    } else if (strcmp(argument, "--version") == 0) {
      puts(PROGRAM " " RYSPEC_VERSION);
      return 0;
    } else if (strcmp(argument, "--dump-symbols") == 0) {
      options.dump_symbols = true;
    } else if (strcmp(argument, "--dump-rules") == 0) {
      options.dump_rules = true;
    } else if (strcmp(argument, "--dump-strings") == 0) {
      options.dump_strings = true;
    } else if (strcmp(argument, "--stats") == 0) {
      options.stats = true;
    } else if (strcmp(argument, "--together") == 0) {
      options.together = true;
    } else if (strcmp(argument, "--corpus") == 0) {
      options.corpus = true;
    } else if (strcmp(argument, "--expect-invalid") == 0) {
      options.expectation = OWES_DIAGNOSTIC;
    } else if (strcmp(argument, "--quiet") == 0) {
      options.quiet = true;
    } else if (strcmp(argument, "--") == 0) {
      first_path = i + 1;
      break;
    } else if (argument[0] == '-' && argument[1] != '\0') {
      fprintf(stderr, PROGRAM ": unknown option %s\n\n%s", argument, USAGE);
      return 2;
    } else {
      first_path = i;
      break;
    }
  }

  if (first_path >= argc) {
    fputs(USAGE, stderr);
    return 2;
  }
  if (options.corpus && options.expectation == OWES_DIAGNOSTIC) {
    fprintf(stderr, PROGRAM ": --corpus and --expect-invalid contradict each other\n");
    return 2;
  }
  if (options.corpus && options.together) {
    fprintf(stderr, PROGRAM ": --corpus is a verdict per file, so it cannot be --together\n");
    return 2;
  }

  return options.together ? run_together(argc, argv, first_path, &options)
                          : run_per_file(argc, argv, first_path, &options);
}
