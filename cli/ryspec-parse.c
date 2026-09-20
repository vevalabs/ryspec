// ryspec-parse -- run the ryspec grammar over documents and report the result.
//
// The parser library answers one question, the first of the three a ryspec
// document has to pass: is this well formed? `ryspec validate` answers the
// other two, the schema's and the loader's, and knows nothing about syntax;
// this is the same tool on the layer below it, and the one place the grammar
// is reachable without Python.
//
//   ryspec-parse data/valid                           every file must parse
//   ryspec-parse --expect-malformed data/malformed    every file must not
//   ryspec-parse --corpus data                        each file's header decides
//   ryspec-parse --print-tree spec.toml               the tree, as an s-expression
//
// --corpus is the corpus contract of data/README.md: a file declaring
// `#:expect-grammar-error` owes the parser a rejection, one declaring
// `#:expect-semantic-error` owes it a clean parse, because semantic invalidity
// is beyond a context-free grammar, and one declaring `#:expect-schema-error`
// owes it nothing either way -- a bad operator is something the grammar is
// entitled to reject and entitled to let through to the schema. A file with no
// header at all is a positive fixture and must parse.
//
// POSIX: directories are walked with dirent.h.

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <tree_sitter/api.h>

// The parser library exports exactly this, and ships no header of its own.
const TSLanguage *tree_sitter_ryspec(void);

#define PROGRAM "ryspec-parse"

// The project version, passed in by the build; the parser's version is the
// grammar's, and the application has no separate one to keep in step.
#ifndef RYSPEC_VERSION
#define RYSPEC_VERSION "unknown"
#endif

static const char USAGE[] =
    "usage: " PROGRAM " [options] <path>...\n"
    "\n"
    "Parse each *.toml file, recursing into any directory given.\n"
    "\n"
    "options:\n"
    "  --expect-malformed  assert every file FAILS to parse (for negative fixtures)\n"
    "  --corpus            let each file's #:expect-<layer>-error header decide\n"
    "  --print-tree        print each parsed file as an s-expression\n"
    "  --quiet             report failures only, not the closing summary\n"
    "  --version           print the version and exit\n"
    "  --help              print this message and exit\n";

// What the grammar owes a file: a clean parse, a rejection, or nothing.
typedef enum { OWES_PARSE, OWES_REJECTION, OWES_NOTHING } Expectation;

typedef struct {
    Expectation expectation;  // what every file owes, unless `corpus` overrides it
    bool corpus;              // take each file's obligation from its own header
    bool print_tree;
    bool quiet;
} Options;

// ---------------------------------------------------------------- file lists

// A growable, sorted list of paths, so a run reports in a stable order.
typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} Paths;

static void *checked(void *pointer) {
    if (pointer == NULL) {
        fprintf(stderr, PROGRAM ": out of memory\n");
        exit(2);
    }
    return pointer;
}

static void paths_push(Paths *paths, const char *path) {
    if (paths->count == paths->capacity) {
        paths->capacity = paths->capacity ? paths->capacity * 2 : 32;
        paths->items = checked(realloc(paths->items, paths->capacity * sizeof *paths->items));
    }
    paths->items[paths->count++] = checked(strdup(path));
}

static void paths_free(Paths *paths) {
    for (size_t i = 0; i < paths->count; i++) free(paths->items[i]);
    free(paths->items);
}

static int compare_paths(const void *left, const void *right) {
    return strcmp(*(const char *const *)left, *(const char *const *)right);
}

static bool has_toml_suffix(const char *name) {
    size_t length = strlen(name);
    return length > 5 && strcmp(name + length - 5, ".toml") == 0;
}

// Every *.toml under `path`, or `path` itself when it is a file. A path named
// on the command line is taken as given; only what a directory walk turns up
// is filtered by suffix, so `ryspec-parse spec.ryspec` still works.
//
// A symlink is followed when it is named and skipped when the walk finds it,
// which is what `grep -r` does and is what keeps a link back up the tree from
// being an unbounded descent.
static bool collect(const char *path, Paths *paths, bool named) {
    struct stat info;
    if ((named ? stat : lstat)(path, &info) != 0) {
        fprintf(stderr, PROGRAM ": %s: %s\n", path, strerror(errno));
        return false;
    }

    if (S_ISLNK(info.st_mode)) return true;
    if (!S_ISDIR(info.st_mode)) {
        if (named || has_toml_suffix(path)) paths_push(paths, path);
        return true;
    }

    DIR *directory = opendir(path);
    if (directory == NULL) {
        fprintf(stderr, PROGRAM ": %s: %s\n", path, strerror(errno));
        return false;
    }

    // The entries of one directory, gathered and sorted before descending, so
    // the walk is deterministic whatever order the filesystem hands them back.
    Paths entries = {0};
    struct dirent *entry;
    bool ok = true;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        size_t size = strlen(path) + strlen(entry->d_name) + 2;
        char *child = checked(malloc(size));
        snprintf(child, size, "%s/%s", path, entry->d_name);
        paths_push(&entries, child);
        free(child);
    }
    closedir(directory);

    qsort(entries.items, entries.count, sizeof *entries.items, compare_paths);
    for (size_t i = 0; i < entries.count; i++) {
        if (!collect(entries.items[i], paths, false)) ok = false;
    }
    paths_free(&entries);
    return ok;
}

static char *read_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, PROGRAM ": %s: %s\n", path, strerror(errno));
        return NULL;
    }

    size_t capacity = 1 << 16, size = 0;
    char *source = checked(malloc(capacity));
    for (;;) {
        if (size == capacity) {
            capacity *= 2;
            source = checked(realloc(source, capacity));
        }
        size_t read = fread(source + size, 1, capacity - size, file);
        size += read;
        if (read == 0) break;
    }
    // The read error, taken before fclose, which is free to set errno itself.
    int failure = ferror(file) ? errno : 0;
    fclose(file);
    if (failure != 0) {
        fprintf(stderr, PROGRAM ": %s: %s\n", path, strerror(failure));
        free(source);
        return NULL;
    }

    *length = size;
    return source;
}

// -------------------------------------------------------------- expectations

// The `#:expect-<layer>-error` header of data/README.md, as an obligation on
// the grammar. A document carries at most one; the Python suite is what holds
// a negative fixture to exactly one, and a second header here would only
// repeat that check further from the corpus it belongs to.
static Expectation declared_expectation(const char *source, size_t length) {
    static const char MARKER[] = "#:expect-";
    static const struct {
        const char *layer;
        Expectation owes;
    } LAYERS[] = {
        {"grammar-error", OWES_REJECTION},
        {"semantic-error", OWES_PARSE},
        {"schema-error", OWES_NOTHING},
    };
    size_t marker_length = sizeof MARKER - 1;

    for (size_t start = 0; start < length;) {
        size_t end = start;
        while (end < length && source[end] != '\n') end++;
        const char *line = source + start;
        size_t line_length = end - start;
        start = end + 1;

        if (line_length <= marker_length || memcmp(line, MARKER, marker_length) != 0) continue;
        const char *layer = line + marker_length;
        size_t rest = line_length - marker_length;
        for (size_t i = 0; i < sizeof LAYERS / sizeof *LAYERS; i++) {
            size_t name_length = strlen(LAYERS[i].layer);
            if (rest >= name_length && memcmp(layer, LAYERS[i].layer, name_length) == 0) {
                return LAYERS[i].owes;
            }
        }
    }
    return OWES_PARSE;
}

// ------------------------------------------------------------------- parsing

// The first ERROR or MISSING node under `node`, or a null node.
//
// A missing hidden token is not reachable through the children, so a tree can
// carry an error this walk cannot name: ts_node_has_error is the verdict, and
// this is only how the report gets a position.
static TSNode first_error(TSNode node) {
    if (strcmp(ts_node_type(node), "ERROR") == 0 || ts_node_is_missing(node)) return node;

    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_child(node, i);
        if (!ts_node_has_error(child)) continue;
        TSNode found = first_error(child);
        if (!ts_node_is_null(found)) return found;
    }
    return (TSNode){{0, 0, 0, 0}, NULL, NULL};
}

// The parse error in `root`, written into `report`, or false if it is clean.
static bool describe_error(TSNode root, char *report, size_t size) {
    if (!ts_node_has_error(root)) return false;

    TSNode node = first_error(root);
    if (ts_node_is_null(node)) {
        snprintf(report, size, "parse error");
        return true;
    }

    TSPoint point = ts_node_start_point(node);
    snprintf(report, size, "%s %s at line %u, column %u",
             ts_node_is_missing(node) ? "missing" : "unexpected", ts_node_type(node),
             point.row + 1, point.column + 1);
    return true;
}

// Parse one file and hold it to what is expected of it. True if it met that.
static bool check(TSParser *parser, const char *path, const Options *options) {
    size_t length = 0;
    char *source = read_file(path, &length);
    if (source == NULL) return false;

    Expectation expectation =
        options->corpus ? declared_expectation(source, length) : options->expectation;

    TSTree *tree = ts_parser_parse_string(parser, NULL, source, (uint32_t)length);
    if (tree == NULL) {
        fprintf(stderr, "FAIL %s: the parser returned no tree\n", path);
        free(source);
        return false;
    }

    TSNode root = ts_tree_root_node(tree);
    char report[256];
    bool rejected = describe_error(root, report, sizeof report);

    if (options->print_tree) {
        char *sexp = ts_node_string(root);
        printf("%s\n%s\n", path, sexp);
        free(sexp);
    }

    bool ok = true;
    if (expectation == OWES_PARSE && rejected) {
        fprintf(stderr, "FAIL %s: %s\n", path, report);
        ok = false;
    } else if (expectation == OWES_REJECTION && !rejected) {
        fprintf(stderr, "FAIL %s: parses cleanly, so it documents no syntax error\n", path);
        ok = false;
    }

    ts_tree_delete(tree);
    free(source);
    return ok;
}

// ---------------------------------------------------------------------- main

int main(int argc, char **argv) {
    Options options = {.expectation = OWES_PARSE};
    int first_path = argc;

    for (int i = 1; i < argc; i++) {
        const char *argument = argv[i];
        if (strcmp(argument, "--help") == 0) {
            fputs(USAGE, stdout);
            return 0;
        } else if (strcmp(argument, "--version") == 0) {
            puts(PROGRAM " " RYSPEC_VERSION);
            return 0;
        } else if (strcmp(argument, "--expect-malformed") == 0) {
            options.expectation = OWES_REJECTION;
        } else if (strcmp(argument, "--corpus") == 0) {
            options.corpus = true;
        } else if (strcmp(argument, "--print-tree") == 0) {
            options.print_tree = true;
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
    if (options.corpus && options.expectation == OWES_REJECTION) {
        fprintf(stderr, PROGRAM ": --corpus and --expect-malformed contradict each other\n");
        return 2;
    }

    Paths paths = {0};
    bool ok = true;
    for (int i = first_path; i < argc; i++) {
        if (!collect(argv[i], &paths, true)) ok = false;
    }
    if (!ok) {
        paths_free(&paths);
        return 2;
    }
    if (paths.count == 0) {
        fprintf(stderr, PROGRAM ": no .toml files found\n");
        paths_free(&paths);
        return 2;
    }

    TSParser *parser = ts_parser_new();
    if (!ts_parser_set_language(parser, tree_sitter_ryspec())) {
        // The runtime rejects a language generated for an ABI it does not
        // speak; nothing about the documents can be said after that.
        fprintf(stderr, PROGRAM ": the runtime cannot load the ryspec parser (ABI mismatch)\n");
        ts_parser_delete(parser);
        paths_free(&paths);
        return 2;
    }

    size_t failures = 0;
    for (size_t i = 0; i < paths.count; i++) {
        if (!check(parser, paths.items[i], &options)) failures++;
    }

    ts_parser_delete(parser);

    if (failures > 0) {
        fprintf(stderr, "\n%zu of %zu file(s) failed\n", failures, paths.count);
    } else if (!options.quiet) {
        // What the run asserted, which is not "parsed" in every mode: under
        // --expect-malformed nothing parsed, and that was the point.
        const char *verdict = options.corpus              ? "matched their expectation"
                              : options.expectation == OWES_REJECTION ? "rejected"
                                                                      : "parsed";
        printf("ok: %zu file(s) %s\n", paths.count, verdict);
    }
    paths_free(&paths);
    return failures > 0 ? 1 : 0;
}
