// ryspec_database -- what a ryspec document says, read out of its parse tree.
//
// The parser answers whether a document is well formed. This answers what it
// means: the names it declares, the rules those names stand for, and whether
// the two hold together. It is the loader of README.md's "what the schema
// cannot check", in C, over the tree rather than over decoded TOML.
//
//   ryspec_database *db = ryspec_database_new();
//   ryspec_database_load(db, "data/valid");
//   ryspec_database_check(db);
//   for (uint32_t i = 0; i < ryspec_diagnostic_count(db); i++) { ... }
//   ryspec_database_free(db);
//
// Three kinds of table live here, and one test sorts every record into the
// right one: a record that carries a position is an entity and is never
// combined by content; a record that carries no position is a value and is
// always deduplicated.
//
//   interned   strings, rules         addressed by content. Immutable, no
//                                     provenance, shared across documents.
//                                     Equal handle means equal content.
//   merged     symbols                addressed by qualified name. One record
//                                     per name ever; a second declaration
//                                     accumulates into the first.
//   appended   bindings, documents,   addressed by insertion order. One record
//              diagnostics            per site, never combined, because each
//                                     answers "where", which a shared value
//                                     cannot.
//
// That is why ryspec_rule has no TSPoint in it, and why a quantifier's bound
// names live in ryspec_binding rather than in the term that binds them.

#ifndef RYSPEC_DATABASE_H_
#define RYSPEC_DATABASE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <tree_sitter/api.h>

#ifdef __cplusplus
extern "C" {
#endif

// ----------------------------------------------------------------- handles

// Every handle is an index, and slot 0 of every table is the absent one: the
// empty string, the null rule, no symbol. So a zeroed struct is an empty one.
typedef uint32_t ryspec_string;       // interned bytes
typedef uint32_t ryspec_name;         // an interned string known to be a name
typedef uint32_t ryspec_rule_id;      // an interned rule term
typedef uint32_t ryspec_symbol_id;
typedef uint32_t ryspec_document_id;

#define RYSPEC_NONE ((uint32_t)0)

// ----------------------------------------------------------------- scalars

typedef enum {
  RYSPEC_VALUE_ABSENT,
  RYSPEC_VALUE_INTEGER,
  RYSPEC_VALUE_FLOAT,
  RYSPEC_VALUE_BOOLEAN,
} ryspec_value_kind;

typedef struct ryspec_value {
  ryspec_value_kind kind;
  union {
    int64_t integer;
    double real;
    bool boolean;
  };
} ryspec_value;

// `number` where a [variables] entry declares no type.
typedef enum {
  RYSPEC_TYPE_NUMBER,
  RYSPEC_TYPE_BOOL,
  RYSPEC_TYPE_TEXT,
  RYSPEC_TYPE_BINARY,
} ryspec_type;

typedef enum {
  RYSPEC_CRITICALITY_NONE,
  RYSPEC_CRITICALITY_INFO,
  RYSPEC_CRITICALITY_WARNING,
  RYSPEC_CRITICALITY_ERROR,
  RYSPEC_CRITICALITY_CRITICAL,
} ryspec_criticality;

// A rule built only from names, comparisons, quantifiers and the boolean
// operators has no direction of its own, which is what lets one shared rule
// serve a past-time property and a future-time one.
typedef enum {
  RYSPEC_CONE_NEUTRAL,
  RYSPEC_CONE_PAST,
  RYSPEC_CONE_FUTURE,
} ryspec_cone;

// --------------------------------------------------------------- operators

// One enum for both spellings: "implies" and "->" are RYSPEC_OP_IMPLIES, "lt"
// and "<" are RYSPEC_OP_LT. That is what lets a prefix rule and its infix twin
// intern to one term.
typedef enum {
  RYSPEC_OP_NONE,
  RYSPEC_OP_LT,
  RYSPEC_OP_LE,
  RYSPEC_OP_GT,
  RYSPEC_OP_GE,
  RYSPEC_OP_EQ,
  RYSPEC_OP_NE,
  RYSPEC_OP_NOT,
  RYSPEC_OP_PREV,
  RYSPEC_OP_NEXT,
  RYSPEC_OP_ONCE,
  RYSPEC_OP_HISTORICALLY,
  RYSPEC_OP_EVENTUALLY,
  RYSPEC_OP_ALWAYS,
  RYSPEC_OP_SINCE,
  RYSPEC_OP_UNTIL,
  RYSPEC_OP_AND,
  RYSPEC_OP_OR,
  RYSPEC_OP_XOR,
  RYSPEC_OP_EQUIV,
  RYSPEC_OP_IMPLIES,
  RYSPEC_OP_FORALL,
  RYSPEC_OP_EXISTS,
} ryspec_operator;

// The cone the operator itself sits in, before its operands are resolved.
ryspec_cone ryspec_operator_cone(ryspec_operator op);
// The prefix spelling, which is the one diagnostics quote.
const char *ryspec_operator_name(ryspec_operator op);

// ------------------------------------------------------------------ bounds

// A prefix `{ min = 3, max = 10 }` and an infix `[3:10]` produce the same
// value. The grammar separates numeric from parametric by node type -- `min`
// against `bound_min` -- so nothing here has to read the text to tell them
// apart.
typedef struct ryspec_bound {
  ryspec_value min;      // ABSENT, or the literal
  ryspec_value max;
  ryspec_name min_name;  // RYSPEC_NONE, or the parameter sizing it
  ryspec_name max_name;
} ryspec_bound;

// ----------------------------------------------------------------- binders

// A reference to a quantifier-bound name, resolved while lowering so that
// alpha-equivalent rules intern to one term: `forall s . P(s)` and
// `forall t . P(t)` are the same rule, and a closed quantified subterm is the
// same wherever it is nested. The level counts binders outward from the use,
// so 0 is the quantifier that binds it -- de Bruijn, relative. Zero as a whole
// is "not bound", which is why the level is stored one higher.
typedef uint32_t ryspec_binder;

#define RYSPEC_BINDER_NONE ((ryspec_binder)0)
#define RYSPEC_BINDER_MAKE(level, index) ((ryspec_binder)((((level) + 1u) << 8) | (index)))
#define RYSPEC_BINDER_LEVEL(binder) (((uint32_t)(binder) >> 8) - 1u)
#define RYSPEC_BINDER_INDEX(binder) ((uint32_t)(binder) & 0xFFu)

// ------------------------------------------------------------------- rules

typedef enum {
  RYSPEC_RULE_REFERENCE,       // a name, already resolved to its qualified form
  RYSPEC_RULE_BOUND_VARIABLE,  // a quantifier-bound name, as a de Bruijn index
  RYSPEC_RULE_PREDICATE,       // lt le gt ge eq ne
  RYSPEC_RULE_UNARY,           // not prev next
  RYSPEC_RULE_UNARY_TEMPORAL,  // once historically eventually always
  RYSPEC_RULE_BINARY_TEMPORAL, // since until
  RYSPEC_RULE_MULTIARY,        // and or xor equiv implies
  RYSPEC_RULE_QUANTIFIED,      // forall exists
} ryspec_rule_kind;

// No TSPoint, and no field outside the key: ryspec_rule_intern hashes the
// whole struct together with the operand run, so there is nothing riding along
// that two equal terms could disagree about. A quantifier keeps only how many
// names it binds -- the spellings are a ryspec_binding -- so `forall s . P(s)`
// and `forall t . P(t)` are one rule.
typedef struct ryspec_rule {
  ryspec_rule_kind kind;
  ryspec_operator op;
  ryspec_bound bound;  // the temporal kinds only

  ryspec_name name;         // REFERENCE: the qualified name
                            // PREDICATE: the left identifier, unless it is bound
  ryspec_name right_name;   // PREDICATE: a named right operand, unless it is bound
  ryspec_value right_value; // PREDICATE: a number, which is an anonymous parameter

  ryspec_binder binder;        // BOUND_VARIABLE: the name this stands for
  ryspec_binder left_binder;   // PREDICATE: set instead of `name` when it is bound
  ryspec_binder right_binder;  // PREDICATE: set instead of `right_name`

  uint32_t operand_first; // into the database's shared operand array
  uint32_t operand_count; // QUANTIFIED: 1. MULTIARY: two or more
  uint32_t var_count;     // QUANTIFIED: how many names it binds
} ryspec_rule;

// ----------------------------------------------------------------- symbols

// Variables, rules and properties share one namespace, so they share one
// table. The roles are a bitset because a name may honestly play two: a
// [rules] definition and the [variables] declaration that publishes it are the
// README's published-name case, and not two definitions of anything. What may
// not be duplicated is a source of value, which is check 11's subject.
typedef enum {
  RYSPEC_ROLE_VARIABLE = 1u << 0,        // a [variables] declaration
  RYSPEC_ROLE_INPUT = 1u << 1,           // listed in [monitor].inputs
  RYSPEC_ROLE_OUTPUT = 1u << 2,
  RYSPEC_ROLE_PARAMETER = 1u << 3,
  RYSPEC_ROLE_RULE = 1u << 4,            // [rules] or [properties.rules]
  RYSPEC_ROLE_PROPERTY = 1u << 5,
  RYSPEC_ROLE_IMPLICIT_INPUT = 1u << 6,  // read by a rule, declared nowhere
} ryspec_role;

typedef struct ryspec_symbol {
  ryspec_name qualified;  // "com.example.plant.guard" -- the table key
  ryspec_name local;      // "guard"
  ryspec_name scope;      // the namespace, or "<namespace>.<property>"
  uint32_t roles;         // a bitset of ryspec_role

  // The declaration, from [variables]. It describes; it defines nothing.
  ryspec_type type;
  bool has_type;
  ryspec_string unit;
  ryspec_string title;
  ryspec_string description;
  ryspec_name source;  // a dotted path into a decoded input
  ryspec_string format;
  ryspec_value initial_value;
  ryspec_value min;
  ryspec_value max;

  // The definition.
  ryspec_rule_id rule;  // ROLE_RULE
  ryspec_rule_id given; // ROLE_PROPERTY
  ryspec_rule_id check;
  ryspec_rule_id impose;
  ryspec_criticality criticality;
  ryspec_string message;

  uint32_t partition_order;  // position within its [monitor] list
  ryspec_document_id document;
  TSPoint point;
  // Where each role was written, so a collision can name both sides.
  TSPoint role_points[7];
} ryspec_symbol;

// --------------------------------------- bindings, documents, diagnostics

// A quantifier site: the names it binds, spelled and placed. The term holds
// only their count, so two alpha-equivalent quantifiers share a term and keep
// separate bindings -- which is what lets a diagnostic say `'s'` at s's line.
typedef struct ryspec_binding {
  ryspec_rule_id rule;  // the QUANTIFIED term
  ryspec_symbol_id owner;
  ryspec_document_id document;
  TSPoint point;
  uint32_t name_first;  // into the database's shared binding-name array
  uint32_t name_count;
  uint32_t enclosing_first;  // the names already bound outside this quantifier
  uint32_t enclosing_count;
  ryspec_name scope;
} ryspec_binding;

typedef struct ryspec_document {
  ryspec_string path;
  ryspec_name namespace_name;  // RYSPEC_NONE where the file declares none
  int64_t version;
  bool has_version;
  // Which [monitor] lists the document wrote, as RYSPEC_ROLE_INPUT, _OUTPUT
  // and _PARAMETER. An absent list and an empty one differ, and this is the
  // only place they do: omitting `parameters` asks the loader to deduce the
  // partition, while `parameters = []` states that there is none. No symbol
  // records that, because an empty list declares no name.
  uint32_t monitor_lists;
  bool parsed;
} ryspec_document;

typedef struct ryspec_diagnostic {
  ryspec_document_id document;
  TSPoint point;
  ryspec_string message;
} ryspec_diagnostic;

typedef struct ryspec_database ryspec_database;

// --------------------------------------------------------------- lifecycle

ryspec_database *ryspec_database_new(void);
void ryspec_database_free(ryspec_database *db);

// Parse `path` -- a file, or every *.toml under a directory, walked and sorted
// the way cli/ryspec-parse.c walks one -- and add what it holds. False when a
// file could not be read or did not parse; a diagnostic records each, and the
// walk carries on, because loading a corpus should report every fault rather
// than the first.
bool ryspec_database_load(ryspec_database *db, const char *path);

// One database per *.toml file under `path`, each parsed and checked, handed
// to `visit` in the order a load would take them. This is for the case a
// single database is wrong for: a corpus of negative fixtures, where each
// file's diagnostic is about that file alone and two of them sharing a name
// would otherwise meet. `visit` owns neither the database nor the path, and
// returning false marks that file failed. False overall where any file could
// not be read, did not parse, or failed its visit.
bool ryspec_each_document(const char *path,
                          bool (*visit)(ryspec_database *db, const char *file, void *context),
                          void *context);

// One document already in memory. The tree is parsed, walked and released
// here; `source` is not retained.
ryspec_document_id ryspec_database_add(ryspec_database *db, const char *path,
                                       const char *source, size_t length);

// Every semantic check over everything loaded, once loading is done. Returns
// the number of errors it added. Calling it twice would report twice.
uint32_t ryspec_database_check(ryspec_database *db);

// ----------------------------------------------------------------- strings

ryspec_string ryspec_intern(ryspec_database *db, const char *text, size_t length);
// `scope + "." + local`, or `local` unchanged where `scope` is absent -- a
// document with no namespace keys its names bare.
ryspec_name ryspec_intern_qualified(ryspec_database *db, ryspec_name scope,
                                    ryspec_name local);
// The bytes behind a handle. NUL-terminated as well, for printf's benefit.
const char *ryspec_text(const ryspec_database *db, ryspec_string handle, size_t *length);
uint32_t ryspec_string_count(const ryspec_database *db);
uint32_t ryspec_string_uses(const ryspec_database *db, ryspec_string handle);

// ----------------------------------------------------------------- symbols

uint32_t ryspec_symbol_count(const ryspec_database *db);
// NULL for RYSPEC_NONE, so a lookup that found nothing reads as one.
const ryspec_symbol *ryspec_symbol_at(const ryspec_database *db, ryspec_symbol_id id);
ryspec_symbol_id ryspec_lookup(const ryspec_database *db, ryspec_name qualified);

// A bare `local` as seen inside a document: the property's own private rule
// first, then the file-level name, then RYSPEC_NONE. `scope` is the property
// whose private table is in view, or RYSPEC_NONE at file scope, and
// `namespace_name` is the document's. Both are needed because both keys carry
// the namespace: one namespace plus a per-property visibility boundary is
// exactly these two steps. Not const: resolving interns the keys it tries.
ryspec_symbol_id ryspec_resolve(ryspec_database *db, ryspec_name namespace_name,
                                ryspec_name scope, ryspec_name local);

// ------------------------------------------------------------------- rules

uint32_t ryspec_rule_count(const ryspec_database *db);
const ryspec_rule *ryspec_rule_at(const ryspec_database *db, ryspec_rule_id id);
const ryspec_rule_id *ryspec_rule_operands(const ryspec_database *db, ryspec_rule_id id,
                                           uint32_t *count);
// The only way to make a rule: an existing id where the database already holds
// this term, a new one otherwise.
ryspec_rule_id ryspec_rule_intern(ryspec_database *db, const ryspec_rule *term,
                                  const ryspec_rule_id *operands, uint32_t operand_count);
// The rule in prefix form. Returns the length it wanted, snprintf-style.
size_t ryspec_rule_format(const ryspec_database *db, ryspec_rule_id id, char *buffer,
                          size_t size);

// ------------------------------------------------ bindings and documents

uint32_t ryspec_binding_count(const ryspec_database *db);
const ryspec_binding *ryspec_binding_at(const ryspec_database *db, uint32_t index);
const ryspec_name *ryspec_binding_names(const ryspec_database *db, uint32_t index,
                                        uint32_t *count);
uint32_t ryspec_document_count(const ryspec_database *db);
const ryspec_document *ryspec_document_at(const ryspec_database *db, ryspec_document_id id);
// Whether the document parsed. One that did not contributes no name and no
// rule, so it has nothing for the checks to say anything about: the grammar's
// verdict is ryspec-parse's to report, not this layer's. A caller with no use
// for the struct can ask this instead.
bool ryspec_document_parsed(const ryspec_database *db, ryspec_document_id id);

// ------------------------------------------------------------- diagnostics

uint32_t ryspec_diagnostic_count(const ryspec_database *db);
const ryspec_diagnostic *ryspec_diagnostic_at(const ryspec_database *db, uint32_t index);
// "path:line:column: message", the line and column 1-based, as
// cli/ryspec-parse.c reports a position. Returns the length it wanted.
size_t ryspec_diagnostic_format(const ryspec_database *db, uint32_t index, char *buffer,
                                size_t size);

#ifdef __cplusplus
}
#endif

#endif  // RYSPEC_DATABASE_H_
