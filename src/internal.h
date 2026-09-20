// Internals shared by database.c, loader.c and semantics.c.
//
// Not installed and not part of the interface: database.h is what a consumer
// gets. This is here because the store, the tree walk and the checks are three
// files by size rather than by boundary -- they are one component, and they
// share the struct.

#ifndef RYSPEC_INTERNAL_H_
#define RYSPEC_INTERNAL_H_

#include "database.h"

// A growable array. `items` is realloc'd, so nothing may hold a pointer into
// one across a push -- indices are the currency here, which is also what the
// handles in database.h are.
#define RYSPEC_VECTOR(type) \
  struct {                  \
    type *items;            \
    uint32_t count;         \
    uint32_t capacity;      \
  }

// These cross translation units but are not interface: database.h is what the
// library promises, and its soname now carries that promise, so everything
// here is hidden rather than exported. Declaring the visibility is enough --
// the definitions inherit it.
#if defined(__GNUC__) || defined(__clang__)
#define RYSPEC_INTERNAL __attribute__((visibility("hidden")))
#else
#define RYSPEC_INTERNAL
#endif

RYSPEC_INTERNAL void *ryspec_checked(void *pointer);
RYSPEC_INTERNAL void *ryspec_grow(void *items, uint32_t *capacity, uint32_t needed, size_t item_size);

#define RYSPEC_PUSH(vector, value)                                                    \
  do {                                                                                \
    (vector).items = ryspec_grow((vector).items, &(vector).capacity, (vector).count + 1, \
                                 sizeof *(vector).items);                             \
    (vector).items[(vector).count++] = (value);                                       \
  } while (0)

// An open-addressing hash index over one of the tables below. A slot holds
// `id + 1`, so 0 is empty and id 0 -- the empty string, the null rule -- is
// still addressable. The capacity is a power of two.
typedef struct {
  uint32_t *slots;
  uint32_t capacity;
} ryspec_index;

RYSPEC_INTERNAL void ryspec_index_reserve(ryspec_index *index, uint32_t count);
RYSPEC_INTERNAL void ryspec_index_place(ryspec_index *index, uint64_t hash, uint32_t id);
RYSPEC_INTERNAL uint64_t ryspec_hash_bytes(const void *bytes, size_t length);
RYSPEC_INTERNAL uint64_t ryspec_hash_u32(uint32_t value);

typedef struct {
  uint32_t offset;  // into the arena
  uint32_t length;
  uint32_t uses;
  uint64_t hash;
} ryspec_string_entry;

struct ryspec_database {
  TSParser *parser;

  // -- interned: addressed by content, immutable, no provenance
  char *arena;
  size_t arena_size;
  size_t arena_capacity;
  RYSPEC_VECTOR(ryspec_string_entry) strings;
  ryspec_index string_index;
  RYSPEC_VECTOR(ryspec_rule) rules;
  RYSPEC_VECTOR(uint64_t) rule_hashes;
  ryspec_index rule_index;
  RYSPEC_VECTOR(ryspec_rule_id) operands;

  // -- merged: addressed by qualified name, accumulating
  RYSPEC_VECTOR(ryspec_symbol) symbols;
  ryspec_index symbol_index;

  // -- appended: one record per site, never combined
  RYSPEC_VECTOR(ryspec_binding) bindings;
  RYSPEC_VECTOR(ryspec_name) binding_names;
  RYSPEC_VECTOR(ryspec_document) documents;
  RYSPEC_VECTOR(ryspec_diagnostic) diagnostics;

  // The resolved cone per rule id, filled by check_time_cones.
  RYSPEC_VECTOR(uint8_t) cones;
};

// Where a role was written, for a diagnostic that has to name both sides of a
// collision. The slot is the role's bit position.
RYSPEC_INTERNAL uint32_t ryspec_role_slot(uint32_t role);

// The merge. Returns the symbol for `qualified`, adding `role` to it, creating
// it only if this is the first time the name has been seen.
RYSPEC_INTERNAL ryspec_symbol_id ryspec_symbol_declare(ryspec_database *db, ryspec_name qualified,
                                       ryspec_name local, ryspec_name scope, uint32_t role,
                                       ryspec_document_id document, TSPoint point);
RYSPEC_INTERNAL ryspec_symbol *ryspec_symbol_mut(ryspec_database *db, ryspec_symbol_id id);

RYSPEC_INTERNAL void ryspec_report(ryspec_database *db, ryspec_document_id document, TSPoint point,
                   const char *format, ...);

// loader.c
RYSPEC_INTERNAL void ryspec_load_tree(ryspec_database *db, ryspec_document_id document, TSNode root,
                      const char *source);

#endif  // RYSPEC_INTERNAL_H_
