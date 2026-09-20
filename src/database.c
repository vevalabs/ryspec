// The store: interning, the symbol table, rule interning, and the tables that
// hold what a shared value cannot -- where each of its uses was written.
//
// Every table here is a growable array with an open-addressing index beside
// it. Nothing holds a pointer across a push; indices are the currency, which
// is what the handles in database.h already are.
//
// POSIX: directories are walked with dirent.h, as cli/ryspec-parse.c walks one.

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "internal.h"

// The parser library exports exactly this, and ships no header of its own.
const TSLanguage *tree_sitter_ryspec(void);

// ------------------------------------------------------------- allocation

void *ryspec_checked(void *pointer) {
  if (pointer == NULL) {
    fprintf(stderr, "ryspec: out of memory\n");
    exit(2);
  }
  return pointer;
}

void *ryspec_grow(void *items, uint32_t *capacity, uint32_t needed, size_t item_size) {
  if (needed <= *capacity) return items;
  uint32_t next = *capacity ? *capacity * 2 : 16;
  while (next < needed) next *= 2;
  *capacity = next;
  return ryspec_checked(realloc(items, (size_t)next * item_size));
}

// ------------------------------------------------------------------ hashes

uint64_t ryspec_hash_bytes(const void *bytes, size_t length) {
  const unsigned char *at = bytes;
  uint64_t hash = 1469598103934665603u;  // FNV-1a
  for (size_t i = 0; i < length; i++) {
    hash ^= at[i];
    hash *= 1099511628211u;
  }
  return hash;
}

uint64_t ryspec_hash_u32(uint32_t value) { return ryspec_hash_bytes(&value, sizeof value); }

void ryspec_index_place(ryspec_index *index, uint64_t hash, uint32_t id) {
  uint32_t mask = index->capacity - 1;
  uint32_t slot = (uint32_t)hash & mask;
  while (index->slots[slot] != 0) slot = (slot + 1) & mask;
  index->slots[slot] = id + 1;
}

// Room for `count` entries at a load factor of 3/4. The caller replaces every
// slot afterwards, because a rehash needs each entry's hash and only the
// caller knows where those live.
void ryspec_index_reserve(ryspec_index *index, uint32_t count) {
  if (index->capacity != 0 && count * 4 < index->capacity * 3) return;
  uint32_t capacity = index->capacity ? index->capacity : 64;
  while (count * 4 >= capacity * 3) capacity *= 2;
  free(index->slots);
  index->slots = ryspec_checked(calloc(capacity, sizeof *index->slots));
  index->capacity = capacity;
}

// ----------------------------------------------------------------- strings

static void string_reindex(ryspec_database *db) {
  ryspec_index_reserve(&db->string_index, db->strings.count + 1);
  for (uint32_t i = 0; i < db->strings.count; i++) {
    ryspec_index_place(&db->string_index, db->strings.items[i].hash, i);
  }
}

ryspec_string ryspec_intern(ryspec_database *db, const char *text, size_t length) {
  if (length == 0) {
    db->strings.items[RYSPEC_NONE].uses++;
    return RYSPEC_NONE;
  }

  uint64_t hash = ryspec_hash_bytes(text, length);
  uint32_t mask = db->string_index.capacity - 1;
  for (uint32_t slot = (uint32_t)hash & mask; db->string_index.slots[slot] != 0;
       slot = (slot + 1) & mask) {
    uint32_t id = db->string_index.slots[slot] - 1;
    ryspec_string_entry *entry = &db->strings.items[id];
    if (entry->hash == hash && entry->length == length &&
        memcmp(db->arena + entry->offset, text, length) == 0) {
      entry->uses++;
      return id;
    }
  }

  // The arena keeps a NUL after every entry, so ryspec_text can hand back a
  // pointer printf is happy with without copying.
  if (db->arena_size + length + 1 > db->arena_capacity) {
    size_t capacity = db->arena_capacity ? db->arena_capacity : 4096;
    while (capacity < db->arena_size + length + 1) capacity *= 2;
    db->arena = ryspec_checked(realloc(db->arena, capacity));
    db->arena_capacity = capacity;
  }
  ryspec_string_entry entry = {
      .offset = (uint32_t)db->arena_size, .length = (uint32_t)length, .uses = 1, .hash = hash};
  memcpy(db->arena + db->arena_size, text, length);
  db->arena[db->arena_size + length] = '\0';
  db->arena_size += length + 1;

  uint32_t id = db->strings.count;
  RYSPEC_PUSH(db->strings, entry);
  if (db->strings.count * 4 >= db->string_index.capacity * 3) {
    string_reindex(db);
  } else {
    ryspec_index_place(&db->string_index, hash, id);
  }
  return id;
}

ryspec_name ryspec_intern_qualified(ryspec_database *db, ryspec_name scope, ryspec_name local) {
  if (scope == RYSPEC_NONE) return local;
  if (local == RYSPEC_NONE) return scope;

  size_t scope_length = 0, local_length = 0;
  const char *scope_text = ryspec_text(db, scope, &scope_length);
  const char *local_text = ryspec_text(db, local, &local_length);

  char stack[256];
  size_t length = scope_length + 1 + local_length;
  char *joined = length < sizeof stack ? stack : ryspec_checked(malloc(length + 1));
  memcpy(joined, scope_text, scope_length);
  joined[scope_length] = '.';
  memcpy(joined + scope_length + 1, local_text, local_length);

  ryspec_name name = ryspec_intern(db, joined, length);
  if (joined != stack) free(joined);
  return name;
}

const char *ryspec_text(const ryspec_database *db, ryspec_string handle, size_t *length) {
  const ryspec_string_entry *entry = &db->strings.items[handle];
  if (length != NULL) *length = entry->length;
  return db->arena + entry->offset;
}

uint32_t ryspec_string_count(const ryspec_database *db) { return db->strings.count; }

uint32_t ryspec_string_uses(const ryspec_database *db, ryspec_string handle) {
  return db->strings.items[handle].uses;
}

// ------------------------------------------------------------------- rules

static bool value_equal(ryspec_value a, ryspec_value b) {
  if (a.kind != b.kind) return false;
  switch (a.kind) {
    case RYSPEC_VALUE_ABSENT: return true;
    case RYSPEC_VALUE_INTEGER: return a.integer == b.integer;
    case RYSPEC_VALUE_FLOAT: return a.real == b.real;
    case RYSPEC_VALUE_BOOLEAN: return a.boolean == b.boolean;
  }
  return false;
}

static void hash_value(uint64_t *hash, ryspec_value value) {
  *hash ^= ryspec_hash_bytes(&value.kind, sizeof value.kind);
  *hash *= 1099511628211u;
  // The union's inactive bytes are indeterminate, so only the live member is
  // hashed -- and compared, in value_equal, for the same reason.
  switch (value.kind) {
    case RYSPEC_VALUE_ABSENT: break;
    case RYSPEC_VALUE_INTEGER: *hash ^= ryspec_hash_bytes(&value.integer, sizeof value.integer); break;
    case RYSPEC_VALUE_FLOAT: *hash ^= ryspec_hash_bytes(&value.real, sizeof value.real); break;
    case RYSPEC_VALUE_BOOLEAN: *hash ^= ryspec_hash_bytes(&value.boolean, sizeof value.boolean); break;
  }
  *hash *= 1099511628211u;
}

static bool bound_equal(ryspec_bound a, ryspec_bound b) {
  return value_equal(a.min, b.min) && value_equal(a.max, b.max) && a.min_name == b.min_name &&
         a.max_name == b.max_name;
}

// Field-wise rather than memcmp: the value union leaves padding a byte compare
// cannot trust, and the whole struct is the key, so nothing is left out.
static uint64_t hash_rule(const ryspec_rule *term, const ryspec_rule_id *operands,
                          uint32_t operand_count) {
  uint64_t hash = 1469598103934665603u;
  uint32_t head[] = {(uint32_t)term->kind, (uint32_t)term->op, term->name,
                     term->right_name,     term->binder,       term->left_binder,
                     term->right_binder,   term->var_count,    operand_count};
  hash ^= ryspec_hash_bytes(head, sizeof head);
  hash *= 1099511628211u;
  hash_value(&hash, term->right_value);
  hash_value(&hash, term->bound.min);
  hash_value(&hash, term->bound.max);
  uint32_t names[] = {term->bound.min_name, term->bound.max_name};
  hash ^= ryspec_hash_bytes(names, sizeof names);
  hash *= 1099511628211u;
  if (operand_count > 0) {
    hash ^= ryspec_hash_bytes(operands, (size_t)operand_count * sizeof *operands);
    hash *= 1099511628211u;
  }
  return hash;
}

static bool rule_equal(const ryspec_database *db, const ryspec_rule *stored,
                       const ryspec_rule *term, const ryspec_rule_id *operands,
                       uint32_t operand_count) {
  if (stored->kind != term->kind || stored->op != term->op || stored->name != term->name ||
      stored->right_name != term->right_name || stored->binder != term->binder ||
      stored->left_binder != term->left_binder || stored->right_binder != term->right_binder ||
      stored->var_count != term->var_count || stored->operand_count != operand_count) {
    return false;
  }
  if (!value_equal(stored->right_value, term->right_value)) return false;
  if (!bound_equal(stored->bound, term->bound)) return false;
  for (uint32_t i = 0; i < operand_count; i++) {
    if (db->operands.items[stored->operand_first + i] != operands[i]) return false;
  }
  return true;
}

static void rule_reindex(ryspec_database *db) {
  ryspec_index_reserve(&db->rule_index, db->rules.count + 1);
  for (uint32_t i = 0; i < db->rules.count; i++) {
    ryspec_index_place(&db->rule_index, db->rule_hashes.items[i], i);
  }
}

ryspec_rule_id ryspec_rule_intern(ryspec_database *db, const ryspec_rule *term,
                                  const ryspec_rule_id *operands, uint32_t operand_count) {
  uint64_t hash = hash_rule(term, operands, operand_count);
  uint32_t mask = db->rule_index.capacity - 1;
  for (uint32_t slot = (uint32_t)hash & mask; db->rule_index.slots[slot] != 0;
       slot = (slot + 1) & mask) {
    uint32_t id = db->rule_index.slots[slot] - 1;
    if (db->rule_hashes.items[id] == hash &&
        rule_equal(db, &db->rules.items[id], term, operands, operand_count)) {
      return id;
    }
  }

  ryspec_rule stored = *term;
  stored.operand_first = db->operands.count;
  stored.operand_count = operand_count;
  for (uint32_t i = 0; i < operand_count; i++) RYSPEC_PUSH(db->operands, operands[i]);

  uint32_t id = db->rules.count;
  RYSPEC_PUSH(db->rules, stored);
  RYSPEC_PUSH(db->rule_hashes, hash);
  if (db->rules.count * 4 >= db->rule_index.capacity * 3) {
    rule_reindex(db);
  } else {
    ryspec_index_place(&db->rule_index, hash, id);
  }
  return id;
}

uint32_t ryspec_rule_count(const ryspec_database *db) { return db->rules.count; }

const ryspec_rule *ryspec_rule_at(const ryspec_database *db, ryspec_rule_id id) {
  return id < db->rules.count ? &db->rules.items[id] : NULL;
}

const ryspec_rule_id *ryspec_rule_operands(const ryspec_database *db, ryspec_rule_id id,
                                           uint32_t *count) {
  const ryspec_rule *term = ryspec_rule_at(db, id);
  if (term == NULL) {
    if (count != NULL) *count = 0;
    return NULL;
  }
  if (count != NULL) *count = term->operand_count;
  return db->operands.items + term->operand_first;
}

// ----------------------------------------------------------------- symbols

uint32_t ryspec_role_slot(uint32_t role) {
  uint32_t slot = 0;
  while ((role >> slot) > 1u) slot++;
  return slot;
}

static void symbol_reindex(ryspec_database *db) {
  ryspec_index_reserve(&db->symbol_index, db->symbols.count + 1);
  for (uint32_t i = 0; i < db->symbols.count; i++) {
    ryspec_index_place(&db->symbol_index, ryspec_hash_u32(db->symbols.items[i].qualified), i);
  }
}

ryspec_symbol_id ryspec_lookup(const ryspec_database *db, ryspec_name qualified) {
  if (db->symbol_index.capacity == 0) return RYSPEC_NONE;
  uint64_t hash = ryspec_hash_u32(qualified);
  uint32_t mask = db->symbol_index.capacity - 1;
  for (uint32_t slot = (uint32_t)hash & mask; db->symbol_index.slots[slot] != 0;
       slot = (slot + 1) & mask) {
    uint32_t id = db->symbol_index.slots[slot] - 1;
    if (db->symbols.items[id].qualified == qualified) return id;
  }
  return RYSPEC_NONE;
}

ryspec_symbol_id ryspec_symbol_declare(ryspec_database *db, ryspec_name qualified,
                                       ryspec_name local, ryspec_name scope, uint32_t role,
                                       ryspec_document_id document, TSPoint point) {
  ryspec_symbol_id id = ryspec_lookup(db, qualified);
  if (id == RYSPEC_NONE) {
    ryspec_symbol symbol = {.qualified = qualified,
                            .local = local,
                            .scope = scope,
                            .document = document,
                            .point = point};
    id = db->symbols.count;
    RYSPEC_PUSH(db->symbols, symbol);
    if (db->symbols.count * 4 >= db->symbol_index.capacity * 3) {
      symbol_reindex(db);
    } else {
      ryspec_index_place(&db->symbol_index, ryspec_hash_u32(qualified), id);
    }
    // Slot 0 of the symbol table is a real symbol, so RYSPEC_NONE has to stay
    // unusable: the loader never asks for the empty name, and a lookup miss
    // returning 0 would otherwise be indistinguishable from finding it. The
    // database reserves entry 0 at construction for exactly this.
  }

  ryspec_symbol *symbol = &db->symbols.items[id];
  if (role != 0 && (symbol->roles & role) == 0) {
    symbol->roles |= role;
    symbol->role_points[ryspec_role_slot(role)] = point;
    // The first role to name a position owns the symbol's own position, so a
    // diagnostic with no better place to point lands on the declaration.
    if (symbol->point.row == 0 && symbol->point.column == 0) symbol->point = point;
  }
  return id;
}

ryspec_symbol *ryspec_symbol_mut(ryspec_database *db, ryspec_symbol_id id) {
  return id != RYSPEC_NONE && id < db->symbols.count ? &db->symbols.items[id] : NULL;
}

// NULL for RYSPEC_NONE, which is what a lookup that found nothing returns.
// Entry 0 of the table exists so that handle 0 stays unusable, and handing it
// back would make "no symbol" indistinguishable from a symbol with no name.
const ryspec_symbol *ryspec_symbol_at(const ryspec_database *db, ryspec_symbol_id id) {
  return id != RYSPEC_NONE && id < db->symbols.count ? &db->symbols.items[id] : NULL;
}

uint32_t ryspec_symbol_count(const ryspec_database *db) { return db->symbols.count; }

ryspec_symbol_id ryspec_resolve(ryspec_database *db, ryspec_name namespace_name,
                                ryspec_name scope, ryspec_name local) {
  // A property's own private rules first, then the file-level name. That is
  // one namespace plus a per-property visibility boundary, and the boundary is
  // in the key rather than in a scope stack.
  if (scope != RYSPEC_NONE && scope != namespace_name) {
    ryspec_symbol_id id = ryspec_lookup(db, ryspec_intern_qualified(db, scope, local));
    if (id != RYSPEC_NONE) return id;
  }
  return ryspec_lookup(db, ryspec_intern_qualified(db, namespace_name, local));
}

// ------------------------------------------- bindings, documents, errors

uint32_t ryspec_binding_count(const ryspec_database *db) { return db->bindings.count; }

const ryspec_binding *ryspec_binding_at(const ryspec_database *db, uint32_t index) {
  return index < db->bindings.count ? &db->bindings.items[index] : NULL;
}

const ryspec_name *ryspec_binding_names(const ryspec_database *db, uint32_t index,
                                        uint32_t *count) {
  const ryspec_binding *binding = ryspec_binding_at(db, index);
  if (binding == NULL) {
    if (count != NULL) *count = 0;
    return NULL;
  }
  if (count != NULL) *count = binding->name_count;
  return db->binding_names.items + binding->name_first;
}

uint32_t ryspec_document_count(const ryspec_database *db) { return db->documents.count; }

const ryspec_document *ryspec_document_at(const ryspec_database *db, ryspec_document_id id) {
  return id < db->documents.count ? &db->documents.items[id] : NULL;
}

bool ryspec_document_parsed(const ryspec_database *db, ryspec_document_id id) {
  const ryspec_document *document = ryspec_document_at(db, id);
  return document != NULL && document->parsed;
}

void ryspec_report(ryspec_database *db, ryspec_document_id document, TSPoint point,
                   const char *format, ...) {
  char message[512];
  va_list arguments;
  va_start(arguments, format);
  int length = vsnprintf(message, sizeof message, format, arguments);
  va_end(arguments);
  if (length < 0) length = 0;
  if ((size_t)length >= sizeof message) length = sizeof message - 1;

  ryspec_diagnostic diagnostic = {.document = document,
                                  .point = point,
                                  .message = ryspec_intern(db, message, (size_t)length)};
  RYSPEC_PUSH(db->diagnostics, diagnostic);
}

uint32_t ryspec_diagnostic_count(const ryspec_database *db) { return db->diagnostics.count; }

const ryspec_diagnostic *ryspec_diagnostic_at(const ryspec_database *db, uint32_t index) {
  return index < db->diagnostics.count ? &db->diagnostics.items[index] : NULL;
}

size_t ryspec_diagnostic_format(const ryspec_database *db, uint32_t index, char *buffer,
                                size_t size) {
  const ryspec_diagnostic *diagnostic = ryspec_diagnostic_at(db, index);
  if (diagnostic == NULL) return 0;
  const ryspec_document *document = ryspec_document_at(db, diagnostic->document);
  const char *path = document != NULL ? ryspec_text(db, document->path, NULL) : "<unknown>";
  int written = snprintf(buffer, size, "%s:%u:%u: %s", path, diagnostic->point.row + 1,
                         diagnostic->point.column + 1, ryspec_text(db, diagnostic->message, NULL));
  return written < 0 ? 0 : (size_t)written;
}

// --------------------------------------------------------------- lifecycle

ryspec_database *ryspec_database_new(void) {
  ryspec_database *db = ryspec_checked(calloc(1, sizeof *db));

  db->parser = ts_parser_new();
  if (!ts_parser_set_language(db->parser, tree_sitter_ryspec())) {
    // The runtime rejects a language generated for an ABI it does not speak;
    // nothing about any document can be said after that.
    fprintf(stderr, "ryspec: the runtime cannot load the ryspec parser (ABI mismatch)\n");
    ts_parser_delete(db->parser);
    free(db);
    return NULL;
  }

  // Slot 0 of every table is the absent one, so a zeroed handle reads as
  // "nothing" everywhere.
  ryspec_index_reserve(&db->string_index, 1);
  ryspec_index_reserve(&db->rule_index, 1);
  ryspec_index_reserve(&db->symbol_index, 1);

  ryspec_string_entry empty = {.offset = 0, .length = 0, .uses = 0,
                               .hash = ryspec_hash_bytes("", 0)};
  db->arena = ryspec_checked(calloc(1, 1));
  db->arena_capacity = 1;
  db->arena_size = 1;
  RYSPEC_PUSH(db->strings, empty);

  ryspec_rule null_rule = {.kind = RYSPEC_RULE_REFERENCE};
  RYSPEC_PUSH(db->rules, null_rule);
  RYSPEC_PUSH(db->rule_hashes, (uint64_t)0);

  ryspec_symbol null_symbol = {0};
  RYSPEC_PUSH(db->symbols, null_symbol);

  ryspec_document null_document = {0};
  RYSPEC_PUSH(db->documents, null_document);
  return db;
}

void ryspec_database_free(ryspec_database *db) {
  if (db == NULL) return;
  ts_parser_delete(db->parser);
  free(db->arena);
  free(db->strings.items);
  free(db->string_index.slots);
  free(db->rules.items);
  free(db->rule_hashes.items);
  free(db->rule_index.slots);
  free(db->operands.items);
  free(db->symbols.items);
  free(db->symbol_index.slots);
  free(db->bindings.items);
  free(db->binding_names.items);
  free(db->documents.items);
  free(db->diagnostics.items);
  free(db->cones.items);
  free(db);
}

// ------------------------------------------------------------------ adding

ryspec_document_id ryspec_database_add(ryspec_database *db, const char *path, const char *source,
                                       size_t length) {
  ryspec_document document = {.path = ryspec_intern(db, path, strlen(path))};
  ryspec_document_id id = db->documents.count;
  RYSPEC_PUSH(db->documents, document);

  TSTree *tree = ts_parser_parse_string(db->parser, NULL, source, (uint32_t)length);
  if (tree == NULL) {
    ryspec_report(db, id, (TSPoint){0, 0}, "the parser returned no tree");
    return id;
  }

  TSNode root = ts_tree_root_node(tree);
  if (ts_node_has_error(root)) {
    // A document that does not parse contributes nothing: every name in it is
    // suspect, and adding half of them would only produce diagnostics about
    // the syntax error in disguise.
    ryspec_report(db, id, ts_node_start_point(root), "does not parse");
    ts_tree_delete(tree);
    return id;
  }

  db->documents.items[id].parsed = true;
  ryspec_load_tree(db, id, root, source);
  ts_tree_delete(tree);
  return id;
}

// -------------------------------------------------------------- file walks

// A growable, sorted list of paths, so a run reports in a stable order. This
// is cli/ryspec-parse.c's walk, with the same symlink rule: followed when the
// path is named, skipped when the walk finds it, which is what keeps a link
// back up the tree from being an unbounded descent.
typedef RYSPEC_VECTOR(char *) paths;

static void paths_push(paths *list, const char *path) {
  RYSPEC_PUSH(*list, ryspec_checked(strdup(path)));
}

static void paths_free(paths *list) {
  for (uint32_t i = 0; i < list->count; i++) free(list->items[i]);
  free(list->items);
}

static int compare_paths(const void *left, const void *right) {
  return strcmp(*(const char *const *)left, *(const char *const *)right);
}

static bool has_toml_suffix(const char *name) {
  size_t length = strlen(name);
  return length > 5 && strcmp(name + length - 5, ".toml") == 0;
}

static bool collect(const char *path, paths *list, bool named) {
  struct stat info;
  if ((named ? stat : lstat)(path, &info) != 0) {
    fprintf(stderr, "ryspec: %s: %s\n", path, strerror(errno));
    return false;
  }

  if (S_ISLNK(info.st_mode)) return true;
  if (!S_ISDIR(info.st_mode)) {
    if (named || has_toml_suffix(path)) paths_push(list, path);
    return true;
  }

  DIR *directory = opendir(path);
  if (directory == NULL) {
    fprintf(stderr, "ryspec: %s: %s\n", path, strerror(errno));
    return false;
  }

  paths entries = {0};
  struct dirent *entry;
  bool ok = true;
  while ((entry = readdir(directory)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    size_t size = strlen(path) + strlen(entry->d_name) + 2;
    char *child = ryspec_checked(malloc(size));
    snprintf(child, size, "%s/%s", path, entry->d_name);
    paths_push(&entries, child);
    free(child);
  }
  closedir(directory);

  qsort(entries.items, entries.count, sizeof *entries.items, compare_paths);
  for (uint32_t i = 0; i < entries.count; i++) {
    if (!collect(entries.items[i], list, false)) ok = false;
  }
  paths_free(&entries);
  return ok;
}

static char *read_file(const char *path, size_t *length) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    fprintf(stderr, "ryspec: %s: %s\n", path, strerror(errno));
    return NULL;
  }

  size_t capacity = 1 << 16, size = 0;
  char *source = ryspec_checked(malloc(capacity));
  for (;;) {
    if (size == capacity) {
      capacity *= 2;
      source = ryspec_checked(realloc(source, capacity));
    }
    size_t read = fread(source + size, 1, capacity - size, file);
    size += read;
    if (read == 0) break;
  }
  // The read error, taken before fclose, which is free to set errno itself.
  int failure = ferror(file) ? errno : 0;
  fclose(file);
  if (failure != 0) {
    fprintf(stderr, "ryspec: %s: %s\n", path, strerror(failure));
    free(source);
    return NULL;
  }

  *length = size;
  return source;
}

bool ryspec_each_document(const char *path,
                          bool (*visit)(ryspec_database *db, const char *file, void *context),
                          void *context) {
  paths list = {0};
  if (!collect(path, &list, true)) {
    paths_free(&list);
    return false;
  }

  bool ok = true;
  for (uint32_t i = 0; i < list.count; i++) {
    size_t length = 0;
    char *source = read_file(list.items[i], &length);
    if (source == NULL) {
      ok = false;
      continue;
    }
    ryspec_database *db = ryspec_database_new();
    if (db == NULL) {
      free(source);
      paths_free(&list);
      return false;
    }
    ryspec_database_add(db, list.items[i], source, length);
    ryspec_database_check(db);
    if (!visit(db, list.items[i], context)) ok = false;
    ryspec_database_free(db);
    free(source);
  }
  paths_free(&list);
  return ok;
}

bool ryspec_database_load(ryspec_database *db, const char *path) {
  paths list = {0};
  if (!collect(path, &list, true)) {
    paths_free(&list);
    return false;
  }

  bool ok = true;
  for (uint32_t i = 0; i < list.count; i++) {
    size_t length = 0;
    char *source = read_file(list.items[i], &length);
    if (source == NULL) {
      ok = false;
      continue;
    }
    ryspec_document_id id = ryspec_database_add(db, list.items[i], source, length);
    if (!db->documents.items[id].parsed) ok = false;
    free(source);
  }
  paths_free(&list);
  return ok;
}
