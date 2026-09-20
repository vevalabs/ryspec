// The checks a document owes beyond its shape.
//
// JSON Schema validates shape, not resolution, so everything that turns on
// what a name reaches is here: README.md's "What the schema cannot check", in
// the order python/src/ryspec/semantics.py runs it, and worded the same, since
// that wording is what python/tests/test_semantics.py and the
// #:expect-semantic-error headers under data/ pin.
//
// Two things the store gives these checks for free. A collision is a symbol
// carrying two roles, because one namespace means one record per name. And the
// cone memo is an array indexed by rule id rather than a map keyed by a
// position, because a term already holds resolved names -- which also means
// the term graph is acyclic, and the only cycle to find is the one a chain of
// names closes.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

// ------------------------------------------------------------------ helpers

static const char *symbol_local(const ryspec_database *db, ryspec_name qualified) {
  ryspec_symbol_id id = ryspec_lookup(db, qualified);
  const ryspec_symbol *symbol = ryspec_symbol_at(db, id);
  return ryspec_text(db, symbol != NULL ? symbol->local : qualified, NULL);
}

static const char *text_of(const ryspec_database *db, ryspec_string handle) {
  return ryspec_text(db, handle, NULL);
}

static bool value_present(ryspec_value value) { return value.kind != RYSPEC_VALUE_ABSENT; }

static double value_number(ryspec_value value) {
  switch (value.kind) {
    case RYSPEC_VALUE_INTEGER: return (double)value.integer;
    case RYSPEC_VALUE_FLOAT: return value.real;
    default: return 0.0;
  }
}

// A number as the Python reporter writes it, so the two diagnostics read the
// same: an integer bare, a float with its fractional part.
static void write_number(char *buffer, size_t size, ryspec_value value) {
  if (value.kind == RYSPEC_VALUE_INTEGER) {
    snprintf(buffer, size, "%lld", (long long)value.integer);
  } else {
    snprintf(buffer, size, "%g", value.real);
  }
}

static bool symbol_in_document(const ryspec_symbol *symbol, ryspec_document_id document) {
  return symbol->document == document;
}

// Every rule a document states: a file-level or private rule's body, and a
// property's given, check and impose.
typedef void (*rule_visitor)(ryspec_database *db, ryspec_symbol_id owner, ryspec_rule_id rule,
                             void *context);

static void for_each_rule_position(ryspec_database *db, rule_visitor visit, void *context) {
  for (ryspec_symbol_id id = 1; id < ryspec_symbol_count(db); id++) {
    const ryspec_symbol *symbol = ryspec_symbol_at(db, id);
    if (symbol->roles & RYSPEC_ROLE_RULE) visit(db, id, symbol->rule, context);
    if (symbol->roles & RYSPEC_ROLE_PROPERTY) {
      visit(db, id, symbol->given, context);
      visit(db, id, symbol->check, context);
      visit(db, id, symbol->impose, context);
    }
  }
}

// ------------------------------------------------- 1. two lists, one name

static void check_monitor_overlap(ryspec_database *db) {
  static const struct {
    uint32_t role;
    const char *name;
  } LISTS[] = {{RYSPEC_ROLE_INPUT, "inputs"},
               {RYSPEC_ROLE_OUTPUT, "outputs"},
               {RYSPEC_ROLE_PARAMETER, "parameters"}};

  for (ryspec_symbol_id id = 1; id < ryspec_symbol_count(db); id++) {
    const ryspec_symbol *symbol = ryspec_symbol_at(db, id);
    for (size_t a = 0; a < 3; a++) {
      if ((symbol->roles & LISTS[a].role) == 0) continue;
      for (size_t b = a + 1; b < 3; b++) {
        if ((symbol->roles & LISTS[b].role) == 0) continue;
        ryspec_report(db, symbol->document, symbol->role_points[ryspec_role_slot(LISTS[b].role)],
                      "'%s' is listed in both '%s' and '%s'", text_of(db, symbol->local),
                      LISTS[a].name, LISTS[b].name);
      }
    }
  }
}

// Check 2, duplicate property names, is the loader's: two [[properties]]
// entries sharing a name merge into one symbol, so the second declaration is
// the only place the fact still exists.

// ------------------------------------ 3. what a [monitor] list may name

static void check_monitor_entries_declared(ryspec_database *db) {
  for (ryspec_symbol_id id = 1; id < ryspec_symbol_count(db); id++) {
    const ryspec_symbol *symbol = ryspec_symbol_at(db, id);
    const char *name = text_of(db, symbol->local);

    if (symbol->roles & (RYSPEC_ROLE_INPUT | RYSPEC_ROLE_PARAMETER)) {
      if ((symbol->roles & RYSPEC_ROLE_VARIABLE) == 0) {
        const char *list = (symbol->roles & RYSPEC_ROLE_INPUT) ? "inputs" : "parameters";
        uint32_t slot = ryspec_role_slot((symbol->roles & RYSPEC_ROLE_INPUT) ? RYSPEC_ROLE_INPUT
                                                                            : RYSPEC_ROLE_PARAMETER);
        ryspec_report(db, symbol->document, symbol->role_points[slot],
                      "'%s' is listed in %s but not declared in [variables]", name, list);
      }
    }

    if ((symbol->roles & RYSPEC_ROLE_OUTPUT) == 0) continue;
    TSPoint point = symbol->role_points[ryspec_role_slot(RYSPEC_ROLE_OUTPUT)];

    // An output publishes a value the file already computes: a [variables]
    // declaration, a property verdict, a file-level rule, or -- qualified --
    // a rule private to a property. The qualified form is the only dotted one
    // a [monitor] list takes, and the schema types it as a dotted path, which
    // admits paths deeper than the single dot this form uses.
    size_t length = 0;
    const char *spelling = ryspec_text(db, symbol->local, &length);
    const char *dot = memchr(spelling, '.', length);
    if (dot == NULL) {
      if ((symbol->roles & (RYSPEC_ROLE_VARIABLE | RYSPEC_ROLE_RULE | RYSPEC_ROLE_PROPERTY)) == 0) {
        ryspec_report(db, symbol->document, point,
                      "'%s' is listed in outputs but names no variable, property or file-level rule",
                      name);
      }
      continue;
    }

    size_t head_length = (size_t)(dot - spelling);
    if (memchr(dot + 1, '.', length - head_length - 1) != NULL) {
      ryspec_report(db, symbol->document, point,
                    "'%s' is listed in outputs but a qualified output names one property and one "
                    "of its rules",
                    name);
      continue;
    }

    // The private rule's key is the namespace in front of what the output
    // already spells, so this is one lookup rather than a scope walk.
    ryspec_name head = ryspec_intern(db, spelling, head_length);
    const ryspec_document *document = ryspec_document_at(db, symbol->document);
    ryspec_name head_qualified = ryspec_intern_qualified(db, document->namespace_name, head);
    const ryspec_symbol *property = ryspec_symbol_at(db, ryspec_lookup(db, head_qualified));
    char property_name[128];
    snprintf(property_name, sizeof property_name, "%.*s", (int)head_length, spelling);

    if (property == NULL || (property->roles & RYSPEC_ROLE_PROPERTY) == 0) {
      ryspec_report(db, symbol->document, point,
                    "'%s' is listed in outputs but no property is named '%s'", name, property_name);
    } else if ((symbol->roles & RYSPEC_ROLE_RULE) == 0) {
      ryspec_report(db, symbol->document, point,
                    "'%s' is listed in outputs but property '%s' declares no rule '%s'", name,
                    property_name, dot + 1);
    }
  }
}

// ------------------------- 4. source and format belong to an input alone

static void check_output_parameter_source_format(ryspec_database *db) {
  for (ryspec_symbol_id id = 1; id < ryspec_symbol_count(db); id++) {
    const ryspec_symbol *symbol = ryspec_symbol_at(db, id);
    if ((symbol->roles & RYSPEC_ROLE_VARIABLE) == 0) continue;  // check 3's problem
    const char *noun = NULL;
    if (symbol->roles & RYSPEC_ROLE_OUTPUT) noun = "an output";
    else if (symbol->roles & RYSPEC_ROLE_PARAMETER) noun = "a parameter";
    if (noun == NULL) continue;

    const char *name = text_of(db, symbol->local);
    if (symbol->source != RYSPEC_NONE) {
      ryspec_report(db, symbol->document, symbol->point, "'%s' is %s and may not declare source",
                    name, noun);
    }
    if (symbol->format != RYSPEC_NONE) {
      ryspec_report(db, symbol->document, symbol->point, "'%s' is %s and may not declare format",
                    name, noun);
    }
  }
}

// ------------------------------ 5. an initial_value is a parameter's alone

static void check_initial_value_without_partition(ryspec_database *db) {
  for (ryspec_document_id doc = 1; doc < ryspec_document_count(db); doc++) {
    // An absent list and an empty one differ, and this is the only place they
    // do: without a `parameters` list there is nothing to have been left out
    // of, because the loader is still free to deduce the partitions. An empty
    // one states that there are none, and then an initial_value has nowhere
    // to belong.
    const ryspec_document *document = ryspec_document_at(db, doc);
    if ((document->monitor_lists & RYSPEC_ROLE_PARAMETER) == 0) continue;

    for (ryspec_symbol_id id = 1; id < ryspec_symbol_count(db); id++) {
      const ryspec_symbol *symbol = ryspec_symbol_at(db, id);
      if (!symbol_in_document(symbol, doc)) continue;
      if (!value_present(symbol->initial_value)) continue;
      uint32_t listed = RYSPEC_ROLE_INPUT | RYSPEC_ROLE_OUTPUT | RYSPEC_ROLE_PARAMETER;
      if ((symbol->roles & listed) != 0) continue;
      ryspec_report(db, symbol->document, symbol->point,
                    "'%s' has an initial_value but is listed in no [monitor] list",
                    text_of(db, symbol->local));
    }
  }
}

// ------------------------------- 6. a parameter cannot be deduced

static void check_parameter_missing_initial_value(ryspec_database *db) {
  for (ryspec_symbol_id id = 1; id < ryspec_symbol_count(db); id++) {
    const ryspec_symbol *symbol = ryspec_symbol_at(db, id);
    if ((symbol->roles & RYSPEC_ROLE_PARAMETER) == 0) continue;
    if ((symbol->roles & RYSPEC_ROLE_VARIABLE) == 0) continue;  // check 3's problem
    if (value_present(symbol->initial_value)) continue;
    ryspec_report(db, symbol->document, symbol->role_points[ryspec_role_slot(RYSPEC_ROLE_PARAMETER)],
                  "parameter '%s' has no initial_value", text_of(db, symbol->local));
  }
}

// ------------------------------------------------------- 7. min over max

static void report_min_max(ryspec_database *db, ryspec_document_id document, TSPoint point,
                           ryspec_value low, ryspec_value high) {
  char low_text[32], high_text[32];
  write_number(low_text, sizeof low_text, low);
  write_number(high_text, sizeof high_text, high);
  ryspec_report(db, document, point, "min (%s) is greater than max (%s)", low_text, high_text);
}

typedef struct {
  uint8_t *seen;
  ryspec_document_id document;
  TSPoint point;
} bound_walk;

static void walk_bounds(ryspec_database *db, ryspec_rule_id id, bound_walk *walk) {
  if (id == RYSPEC_NONE || walk->seen[id]) return;
  walk->seen[id] = 1;
  const ryspec_rule *term = ryspec_rule_at(db, id);
  if (term == NULL) return;
  if (value_present(term->bound.min) && value_present(term->bound.max) &&
      value_number(term->bound.min) > value_number(term->bound.max)) {
    report_min_max(db, walk->document, walk->point, term->bound.min, term->bound.max);
  }
  uint32_t count = 0;
  const ryspec_rule_id *operands = ryspec_rule_operands(db, id, &count);
  for (uint32_t i = 0; i < count; i++) walk_bounds(db, operands[i], walk);
}

static void visit_bounds(ryspec_database *db, ryspec_symbol_id owner, ryspec_rule_id rule,
                         void *context) {
  const ryspec_symbol *symbol = ryspec_symbol_at(db, owner);
  bound_walk walk = {.seen = context, .document = symbol->document, .point = symbol->point};
  walk_bounds(db, rule, &walk);
}

static void check_min_max(ryspec_database *db) {
  for (ryspec_symbol_id id = 1; id < ryspec_symbol_count(db); id++) {
    const ryspec_symbol *symbol = ryspec_symbol_at(db, id);
    if (!value_present(symbol->min) || !value_present(symbol->max)) continue;
    if (value_number(symbol->min) <= value_number(symbol->max)) continue;
    report_min_max(db, symbol->document, symbol->point, symbol->min, symbol->max);
  }

  uint8_t *seen = ryspec_checked(calloc(ryspec_rule_count(db) + 1, 1));
  for_each_rule_position(db, visit_bounds, seen);
  free(seen);
}

// -------------------------------------------------------- 8. source paths

static void check_source_paths(ryspec_database *db) {
  for (ryspec_symbol_id id = 1; id < ryspec_symbol_count(db); id++) {
    const ryspec_symbol *symbol = ryspec_symbol_at(db, id);
    if (symbol->source == RYSPEC_NONE) continue;

    size_t length = 0;
    const char *source = ryspec_text(db, symbol->source, &length);
    const char *dot = memchr(source, '.', length);
    size_t head_length = dot != NULL ? (size_t)(dot - source) : length;

    const ryspec_document *document = ryspec_document_at(db, symbol->document);
    ryspec_name head = ryspec_intern(db, source, head_length);
    ryspec_name qualified = ryspec_intern_qualified(db, document->namespace_name, head);
    const ryspec_symbol *target = ryspec_symbol_at(db, ryspec_lookup(db, qualified));
    char head_name[128];
    snprintf(head_name, sizeof head_name, "%.*s", (int)head_length, source);

    if (target == NULL || (target->roles & RYSPEC_ROLE_VARIABLE) == 0) {
      ryspec_report(db, symbol->document, symbol->point,
                    "source '%s' refers to undeclared variable '%s'", source, head_name);
    } else if (dot != NULL && target->format == RYSPEC_NONE) {
      // A dotted path reaches into a decoded document, which only a declared
      // format says how to read.
      ryspec_report(db, symbol->document, symbol->point,
                    "source '%s' points to '%s', which declares no format", source, head_name);
    }
  }
}

// ---------------------------------- 9. a rule over a text or binary value

typedef struct {
  uint8_t *seen;
  ryspec_document_id document;
  TSPoint point;
} type_walk;

static void check_named_value(ryspec_database *db, ryspec_name qualified, type_walk *walk) {
  if (qualified == RYSPEC_NONE) return;
  const ryspec_symbol *symbol = ryspec_symbol_at(db, ryspec_lookup(db, qualified));
  if (symbol == NULL || !symbol->has_type) return;
  if (symbol->type != RYSPEC_TYPE_TEXT && symbol->type != RYSPEC_TYPE_BINARY) return;
  ryspec_report(db, walk->document, walk->point,
                "rule refers to '%s', a %s variable with no comparable value",
                text_of(db, symbol->local),
                symbol->type == RYSPEC_TYPE_TEXT ? "text" : "binary");
}

static void walk_types(ryspec_database *db, ryspec_rule_id id, type_walk *walk) {
  if (id == RYSPEC_NONE || walk->seen[id]) return;
  walk->seen[id] = 1;
  const ryspec_rule *term = ryspec_rule_at(db, id);
  if (term == NULL) return;
  if (term->kind == RYSPEC_RULE_REFERENCE || term->kind == RYSPEC_RULE_PREDICATE) {
    check_named_value(db, term->name, walk);
    check_named_value(db, term->right_name, walk);
  }
  // A bound's end is a number or a name resolving to a parameter, and a
  // document is not a distance in steps.
  check_named_value(db, term->bound.min_name, walk);
  check_named_value(db, term->bound.max_name, walk);
  uint32_t count = 0;
  const ryspec_rule_id *operands = ryspec_rule_operands(db, id, &count);
  for (uint32_t i = 0; i < count; i++) walk_types(db, operands[i], walk);
}

static void visit_types(ryspec_database *db, ryspec_symbol_id owner, ryspec_rule_id rule,
                        void *context) {
  const ryspec_symbol *symbol = ryspec_symbol_at(db, owner);
  type_walk walk = {.seen = context, .document = symbol->document, .point = symbol->point};
  walk_types(db, rule, &walk);
}

static void check_rule_names_text_binary(ryspec_database *db) {
  uint8_t *seen = ryspec_checked(calloc(ryspec_rule_count(db) + 1, 1));
  for_each_rule_position(db, visit_types, seen);
  free(seen);
}

// --------------------------------------------------------- 10. quantifiers

// How a bound name is read inside the rule it quantifies. It denotes a value
// drawn from the trace, so it belongs on either side of a comparison and
// nowhere else.
typedef enum { USE_NONE, USE_VALUE, USE_RULE, USE_BOUND } binder_use;

static void binder_uses(ryspec_database *db, ryspec_rule_id id, uint32_t level,
                        const ryspec_name *names, uint32_t count, binder_use *worst,
                        uint8_t *seen) {
  if (id == RYSPEC_NONE || seen[id]) return;
  seen[id] = 1;
  const ryspec_rule *term = ryspec_rule_at(db, id);
  if (term == NULL) return;

  if (term->kind == RYSPEC_RULE_BOUND_VARIABLE && term->binder != RYSPEC_BINDER_NONE &&
      RYSPEC_BINDER_LEVEL(term->binder) == level) {
    uint32_t index = RYSPEC_BINDER_INDEX(term->binder);
    if (index < count && worst[index] < USE_RULE) worst[index] = USE_RULE;
  }
  if (term->kind == RYSPEC_RULE_PREDICATE) {
    ryspec_binder sides[2] = {term->left_binder, term->right_binder};
    for (size_t i = 0; i < 2; i++) {
      if (sides[i] == RYSPEC_BINDER_NONE || RYSPEC_BINDER_LEVEL(sides[i]) != level) continue;
      uint32_t index = RYSPEC_BINDER_INDEX(sides[i]);
      if (index < count && worst[index] == USE_NONE) worst[index] = USE_VALUE;
    }
  }
  // A bound is fixed before the run, so it names a parameter rather than a
  // quantified value; the name survives lowering, which is how it is caught.
  ryspec_name bound_names[2] = {term->bound.min_name, term->bound.max_name};
  for (size_t i = 0; i < 2; i++) {
    if (bound_names[i] == RYSPEC_NONE) continue;
    for (uint32_t index = 0; index < count; index++) {
      const ryspec_symbol *symbol = ryspec_symbol_at(db, ryspec_lookup(db, bound_names[i]));
      ryspec_name local = symbol != NULL ? symbol->local : bound_names[i];
      if (local == names[index] && worst[index] < USE_BOUND) worst[index] = USE_BOUND;
    }
  }

  uint32_t operand_count = 0;
  const ryspec_rule_id *operands = ryspec_rule_operands(db, id, &operand_count);
  // A nested quantifier puts one more binder between the use and this one.
  uint32_t inner = term->kind == RYSPEC_RULE_QUANTIFIED ? level + 1 : level;
  for (uint32_t i = 0; i < operand_count; i++) {
    binder_uses(db, operands[i], inner, names, count, worst, seen);
  }
}

static void check_quantifiers(ryspec_database *db) {
  for (uint32_t index = 0; index < ryspec_binding_count(db); index++) {
    const ryspec_binding *binding = ryspec_binding_at(db, index);
    uint32_t count = 0;
    const ryspec_name *names = ryspec_binding_names(db, index, &count);
    if (count == 0) continue;

    uint32_t operand_count = 0;
    const ryspec_rule_id *operands = ryspec_rule_operands(db, binding->rule, &operand_count);
    ryspec_rule_id operand = operand_count > 0 ? operands[0] : RYSPEC_NONE;

    // Sized to the loader's RYSPEC_MAX_BINDER_VARS, which caps `count`.
    binder_use worst[8] = {USE_NONE};
    uint8_t *seen = ryspec_checked(calloc(ryspec_rule_count(db) + 1, 1));
    binder_uses(db, operand, 0, names, count, worst, seen);
    free(seen);

    for (uint32_t i = 0; i < count; i++) {
      const char *name = text_of(db, names[i]);

      bool enclosed = false;
      for (uint32_t j = 0; j < binding->enclosing_count; j++) {
        if (ryspec_binding_names(db, index, NULL) == NULL) break;
        if (db->binding_names.items[binding->enclosing_first + j] == names[i]) enclosed = true;
      }
      if (enclosed) {
        ryspec_report(db, binding->document, binding->point,
                      "quantifier variable '%s' is already bound by an enclosing quantifier", name);
        continue;
      }

      // A quantified name is declared in no table, so anything it does reach
      // is a name it shadows.
      const ryspec_document *document = ryspec_document_at(db, binding->document);
      if (ryspec_resolve(db, document->namespace_name, binding->scope, names[i]) != RYSPEC_NONE) {
        ryspec_report(db, binding->document, binding->point,
                      "quantifier variable '%s' shadows a declared name", name);
        continue;
      }

      switch (worst[i]) {
        case USE_NONE:
          // Quantification does not reach through a name, so a quantifier over
          // a bare reference binds something that rule never sees.
          ryspec_report(db, binding->document, binding->point,
                        "quantifier variable '%s' is never used in the rule it quantifies", name);
          break;
        case USE_RULE:
          ryspec_report(db, binding->document, binding->point,
                        "quantifier variable '%s' stands where a rule is expected, but a "
                        "quantifier variable is a value rather than a proposition",
                        name);
          break;
        case USE_BOUND:
          ryspec_report(db, binding->document, binding->point,
                        "quantifier variable '%s' sizes a metric bound, but a bound is fixed "
                        "before the run and a quantifier variable is not",
                        name);
          break;
        case USE_VALUE: break;
      }
    }
  }
}

// ------------------------------------------- 11. two sources of one value

// What a role is called where a collision names both sides, in the order the
// message lists them. A [variables] declaration is not here: it describes a
// value, and describing is not producing one -- which is exactly what lets a
// published rule and its declaration share a name.
static const struct {
  uint32_t role;
  const char *kind;
} VALUE_ROLES[] = {{RYSPEC_ROLE_RULE, "rule"},
                   {RYSPEC_ROLE_PROPERTY, "property"},
                   {RYSPEC_ROLE_INPUT, "input"},
                   {RYSPEC_ROLE_OUTPUT, "output"},
                   {RYSPEC_ROLE_PARAMETER, "parameter"},
                   {RYSPEC_ROLE_IMPLICIT_INPUT, "implicit_input"}};

// A name in two [monitor] lists at once is check 1's to report, not a second
// entry here, so at most one list counts.
static uint32_t value_roles(uint32_t roles) {
  uint32_t monitor = roles & (RYSPEC_ROLE_INPUT | RYSPEC_ROLE_OUTPUT | RYSPEC_ROLE_PARAMETER);
  if (monitor != 0 && (monitor & (monitor - 1)) != 0) {
    // Keep the first: inputs, then outputs, then parameters.
    if (monitor & RYSPEC_ROLE_INPUT) monitor = RYSPEC_ROLE_INPUT;
    else monitor = RYSPEC_ROLE_OUTPUT;
  }
  return (roles & (RYSPEC_ROLE_RULE | RYSPEC_ROLE_PROPERTY | RYSPEC_ROLE_IMPLICIT_INPUT)) | monitor;
}

// An output may coincide with exactly one rule or property of that name -- the
// sanctioned "publish this rule's value". inputs and parameters grant no such
// licence: an input arrives with a value and a parameter carries an
// initial_value, so either is a source of value in its own right.
static bool roles_sanctioned(uint32_t roles) {
  return roles == (RYSPEC_ROLE_OUTPUT | RYSPEC_ROLE_RULE) ||
         roles == (RYSPEC_ROLE_OUTPUT | RYSPEC_ROLE_PROPERTY);
}

static uint32_t describe_roles(uint32_t roles, char *buffer, size_t size, bool extra_rule) {
  size_t at = 0;
  uint32_t count = 0;
  for (size_t i = 0; i < sizeof VALUE_ROLES / sizeof *VALUE_ROLES; i++) {
    if ((roles & VALUE_ROLES[i].role) == 0) continue;
    at += (size_t)snprintf(buffer + at, at < size ? size - at : 0, "%s%s", count ? ", " : "",
                           VALUE_ROLES[i].kind);
    count++;
  }
  if (extra_rule) {
    at += (size_t)snprintf(buffer + at, at < size ? size - at : 0, "%srule", count ? ", " : "");
    count++;
  }
  return count;
}

static void check_source_of_value_collisions(ryspec_database *db) {
  // Reported once per name, as the Python reporter does, so a name that
  // collides at file scope is not reported again inside a property.
  uint8_t *reported = ryspec_checked(calloc(ryspec_symbol_count(db) + 1, 1));

  for (ryspec_symbol_id id = 1; id < ryspec_symbol_count(db); id++) {
    const ryspec_symbol *symbol = ryspec_symbol_at(db, id);
    const ryspec_document *document = ryspec_document_at(db, symbol->document);
    if (symbol->scope != document->namespace_name) continue;  // a private rule: below

    uint32_t roles = value_roles(symbol->roles);
    char kinds[128];
    uint32_t count = describe_roles(roles, kinds, sizeof kinds, false);
    if (count < 2 || roles_sanctioned(roles)) continue;
    reported[id] = 1;
    ryspec_report(db, symbol->document, symbol->point,
                  "'%s' names more than one source of value (%s)", text_of(db, symbol->local),
                  kinds);
  }

  // A rule private to a property is a visibility boundary, not a licence: two
  // properties may each declare their own `rhs` without colliding, but a
  // private rule still meets whatever the file scope already declares under
  // that name. An implicit input is the exception -- inside the property the
  // private rule is what the name reaches, so it was never implicit there.
  for (ryspec_symbol_id id = 1; id < ryspec_symbol_count(db); id++) {
    const ryspec_symbol *symbol = ryspec_symbol_at(db, id);
    const ryspec_document *document = ryspec_document_at(db, symbol->document);
    if (symbol->scope == document->namespace_name) continue;
    if ((symbol->roles & RYSPEC_ROLE_RULE) == 0) continue;

    ryspec_name qualified = ryspec_intern_qualified(db, document->namespace_name, symbol->local);
    ryspec_symbol_id outer_id = ryspec_lookup(db, qualified);
    const ryspec_symbol *outer = ryspec_symbol_at(db, outer_id);
    if (outer == NULL || reported[outer_id]) continue;

    uint32_t roles = value_roles(outer->roles) & ~(uint32_t)RYSPEC_ROLE_IMPLICIT_INPUT;
    char kinds[128];
    uint32_t count = describe_roles(roles, kinds, sizeof kinds, true);
    if (count < 2) continue;
    if (roles_sanctioned(roles)) continue;
    reported[outer_id] = 1;
    ryspec_report(db, symbol->document, symbol->point,
                  "'%s' names more than one source of value (%s)", text_of(db, symbol->local),
                  kinds);
  }

  free(reported);
}

// ------------------------------------------------------- 12. time cones

// The cone of every rule, resolved through the names it reads. The schema
// settles a rule written out in full -- ["always", ["once", "p"]] is rejected
// there -- and a name hides one, which is what this follows.
typedef struct {
  uint8_t *state;  // 0 unknown, 1 resolving, 2 done
  ryspec_symbol_id *stack;
  uint32_t depth;
  ryspec_document_id document;
  TSPoint point;
} cone_walk;

static ryspec_cone cone_of_rule(ryspec_database *db, ryspec_rule_id id, cone_walk *walk);

// How a diagnostic names a sub-rule: by name where it has one.
static void describe_rule(ryspec_database *db, ryspec_rule_id id, char *buffer, size_t size) {
  const ryspec_rule *term = ryspec_rule_at(db, id);
  if (term != NULL && term->kind == RYSPEC_RULE_REFERENCE && term->name != RYSPEC_NONE) {
    snprintf(buffer, size, "'%s'", symbol_local(db, term->name));
  } else {
    snprintf(buffer, size, "the operand");
  }
}

static const char *cone_name(ryspec_cone cone) {
  return cone == RYSPEC_CONE_PAST ? "past" : "future";
}

static ryspec_cone cone_of_symbol(ryspec_database *db, ryspec_symbol_id id, cone_walk *walk) {
  const ryspec_symbol *symbol = ryspec_symbol_at(db, id);
  if (symbol == NULL) return RYSPEC_CONE_NEUTRAL;
  // A name that resolves to a property reaches that property's verdict, which
  // is its check: given gates the verdict rather than computing it, so it
  // lends it no cone.
  ryspec_rule_id rule = (symbol->roles & RYSPEC_ROLE_RULE) ? symbol->rule
                        : (symbol->roles & RYSPEC_ROLE_PROPERTY) ? symbol->check
                                                                 : RYSPEC_NONE;
  if (rule == RYSPEC_NONE) return RYSPEC_CONE_NEUTRAL;

  for (uint32_t i = 0; i < walk->depth; i++) {
    if (walk->stack[i] == id) {
      ryspec_report(db, walk->document, walk->point,
                    "'%s' is defined in terms of itself, so it has no value",
                    ryspec_text(db, symbol->local, NULL));
      return RYSPEC_CONE_NEUTRAL;
    }
  }
  walk->stack[walk->depth++] = id;
  ryspec_cone cone = cone_of_rule(db, rule, walk);
  walk->depth--;
  return cone;
}

static ryspec_cone cone_of_rule(ryspec_database *db, ryspec_rule_id id, cone_walk *walk) {
  if (id == RYSPEC_NONE) return RYSPEC_CONE_NEUTRAL;
  if (walk->state[id] == 2) return (ryspec_cone)db->cones.items[id];

  const ryspec_rule *term = ryspec_rule_at(db, id);
  if (term == NULL) return RYSPEC_CONE_NEUTRAL;

  uint32_t count = 0;
  const ryspec_rule_id *operands = ryspec_rule_operands(db, id, &count);
  ryspec_cone cone = RYSPEC_CONE_NEUTRAL;

  switch (term->kind) {
    case RYSPEC_RULE_REFERENCE:
      cone = cone_of_symbol(db, ryspec_lookup(db, term->name), walk);
      break;
    case RYSPEC_RULE_BOUND_VARIABLE:
    case RYSPEC_RULE_PREDICATE:
      cone = RYSPEC_CONE_NEUTRAL;
      break;
    case RYSPEC_RULE_UNARY:
    case RYSPEC_RULE_QUANTIFIED:
      // `not`, `forall` and `exists` carry their operand's cone. `prev` and
      // `next` are unary_operator in the grammar but temporal in meaning.
      cone = ryspec_operator_cone(term->op);
      if (cone == RYSPEC_CONE_NEUTRAL && count > 0) cone = cone_of_rule(db, operands[0], walk);
      break;
    case RYSPEC_RULE_MULTIARY: {
      // A boolean combination takes its operands' cone where they agree.
      ryspec_cone settled = RYSPEC_CONE_NEUTRAL;
      ryspec_rule_id settled_rule = RYSPEC_NONE;
      for (uint32_t i = 0; i < count; i++) {
        ryspec_cone operand = cone_of_rule(db, operands[i], walk);
        if (operand == RYSPEC_CONE_NEUTRAL) continue;
        if (settled == RYSPEC_CONE_NEUTRAL) {
          settled = operand;
          settled_rule = operands[i];
        } else if (operand != settled) {
          char described[128], settled_described[128];
          describe_rule(db, operands[i], described, sizeof described);
          describe_rule(db, settled_rule, settled_described, sizeof settled_described);
          ryspec_report(db, walk->document, walk->point,
                        "%s is a %s-time rule and the operand at %s is a %s-time rule", described,
                        cone_name(operand), settled_described, cone_name(settled));
        }
      }
      cone = settled;
      break;
    }
    case RYSPEC_RULE_UNARY_TEMPORAL:
    case RYSPEC_RULE_BINARY_TEMPORAL: {
      ryspec_cone own = ryspec_operator_cone(term->op);
      ryspec_cone opposite = own == RYSPEC_CONE_PAST ? RYSPEC_CONE_FUTURE : RYSPEC_CONE_PAST;
      for (uint32_t i = 0; i < count; i++) {
        if (cone_of_rule(db, operands[i], walk) != opposite) continue;
        char described[128];
        describe_rule(db, operands[i], described, sizeof described);
        ryspec_report(db, walk->document, walk->point,
                      "'%s' looks to the %s and %s is a %s-time rule",
                      ryspec_operator_name(term->op), cone_name(own), described,
                      cone_name(opposite));
      }
      cone = own;
      break;
    }
  }

  // Memoised on rule id: a term already holds resolved names, so its cone does
  // not depend on where it was read.
  if (walk->depth == 0 || term->kind != RYSPEC_RULE_REFERENCE) {
    db->cones.items[id] = (uint8_t)cone;
    walk->state[id] = 2;
  }
  return cone;
}

static void visit_cone(ryspec_database *db, ryspec_symbol_id owner, ryspec_rule_id rule,
                       void *context) {
  if (rule == RYSPEC_NONE) return;
  cone_walk *walk = context;
  const ryspec_symbol *symbol = ryspec_symbol_at(db, owner);
  walk->document = symbol->document;
  walk->point = symbol->point;
  walk->depth = 0;
  walk->stack[walk->depth++] = owner;
  cone_of_rule(db, rule, walk);
  walk->depth--;
}

static void check_time_cones(ryspec_database *db) {
  uint32_t rules = ryspec_rule_count(db);
  db->cones.items = ryspec_grow(db->cones.items, &db->cones.capacity, rules + 1, 1);
  memset(db->cones.items, 0, rules + 1);
  db->cones.count = rules + 1;

  cone_walk walk = {
      .state = ryspec_checked(calloc(rules + 1, 1)),
      .stack = ryspec_checked(calloc(ryspec_symbol_count(db) + 1, sizeof(ryspec_symbol_id)))};
  for_each_rule_position(db, visit_cone, &walk);
  free(walk.state);
  free(walk.stack);
}

// ------------------------------------------------------------------- all

uint32_t ryspec_database_check(ryspec_database *db) {
  uint32_t before = ryspec_diagnostic_count(db);
  check_monitor_overlap(db);
  check_monitor_entries_declared(db);
  check_output_parameter_source_format(db);
  check_initial_value_without_partition(db);
  check_parameter_missing_initial_value(db);
  check_min_max(db);
  check_source_paths(db);
  check_rule_names_text_binary(db);
  check_quantifiers(db);
  check_source_of_value_collisions(db);
  check_time_cones(db);
  return ryspec_diagnostic_count(db) - before;
}

// ------------------------------------------------------- printing a rule

static size_t append(char *buffer, size_t size, size_t at, const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  int written = vsnprintf(buffer + (at < size ? at : size), at < size ? size - at : 0, format,
                          arguments);
  va_end(arguments);
  return at + (written < 0 ? 0 : (size_t)written);
}

static size_t format_value(const ryspec_database *db, ryspec_value value, char *buffer, size_t size,
                           size_t at) {
  (void)db;
  char text[32];
  write_number(text, sizeof text, value);
  return append(buffer, size, at, "%s", text);
}

static size_t format_bound(const ryspec_database *db, ryspec_bound bound, char *buffer, size_t size,
                           size_t at) {
  bool any = value_present(bound.min) || value_present(bound.max) ||
             bound.min_name != RYSPEC_NONE || bound.max_name != RYSPEC_NONE;
  if (!any) return at;
  at = append(buffer, size, at, ", {");
  bool first = true;
  if (value_present(bound.min)) {
    at = append(buffer, size, at, "min = ");
    at = format_value(db, bound.min, buffer, size, at);
    first = false;
  } else if (bound.min_name != RYSPEC_NONE) {
    at = append(buffer, size, at, "min = \"%s\"", symbol_local(db, bound.min_name));
    first = false;
  }
  if (value_present(bound.max)) {
    at = append(buffer, size, at, "%smax = ", first ? "" : ", ");
    at = format_value(db, bound.max, buffer, size, at);
  } else if (bound.max_name != RYSPEC_NONE) {
    at = append(buffer, size, at, "%smax = \"%s\"", first ? "" : ", ",
                symbol_local(db, bound.max_name));
  }
  return append(buffer, size, at, "}");
}

static size_t format_rule(const ryspec_database *db, ryspec_rule_id id, char *buffer, size_t size,
                          size_t at) {
  const ryspec_rule *term = ryspec_rule_at(db, id);
  if (term == NULL || id == RYSPEC_NONE) return append(buffer, size, at, "-");

  switch (term->kind) {
    case RYSPEC_RULE_REFERENCE:
      return append(buffer, size, at, "\"%s\"", ryspec_text(db, term->name, NULL));
    case RYSPEC_RULE_BOUND_VARIABLE:
      return append(buffer, size, at, "$%u.%u", RYSPEC_BINDER_LEVEL(term->binder),
                    RYSPEC_BINDER_INDEX(term->binder));
    case RYSPEC_RULE_PREDICATE: {
      at = append(buffer, size, at, "[\"%s\", ", ryspec_operator_name(term->op));
      if (term->left_binder != RYSPEC_BINDER_NONE) {
        at = append(buffer, size, at, "$%u.%u", RYSPEC_BINDER_LEVEL(term->left_binder),
                    RYSPEC_BINDER_INDEX(term->left_binder));
      } else {
        at = append(buffer, size, at, "\"%s\"", ryspec_text(db, term->name, NULL));
      }
      at = append(buffer, size, at, ", ");
      if (term->right_binder != RYSPEC_BINDER_NONE) {
        at = append(buffer, size, at, "$%u.%u", RYSPEC_BINDER_LEVEL(term->right_binder),
                    RYSPEC_BINDER_INDEX(term->right_binder));
      } else if (value_present(term->right_value)) {
        at = format_value(db, term->right_value, buffer, size, at);
      } else {
        at = append(buffer, size, at, "\"%s\"", ryspec_text(db, term->right_name, NULL));
      }
      return append(buffer, size, at, "]");
    }
    default: break;
  }

  at = append(buffer, size, at, "[\"%s\"", ryspec_operator_name(term->op));
  uint32_t count = 0;
  const ryspec_rule_id *operands = ryspec_rule_operands(db, id, &count);
  for (uint32_t i = 0; i < count; i++) {
    at = append(buffer, size, at, ", ");
    at = format_rule(db, operands[i], buffer, size, at);
  }
  if (term->kind == RYSPEC_RULE_QUANTIFIED) {
    at = append(buffer, size, at, ", {vars = %u}", term->var_count);
  }
  at = format_bound(db, term->bound, buffer, size, at);
  return append(buffer, size, at, "]");
}

size_t ryspec_rule_format(const ryspec_database *db, ryspec_rule_id id, char *buffer, size_t size) {
  size_t length = format_rule(db, id, buffer, size, 0);
  if (size > 0) buffer[length < size ? length : size - 1] = '\0';
  return length;
}
