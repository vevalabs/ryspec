// The tree walk: what a parse tree says, put into the store.
//
// Two passes over the document's children. The first declares every name, so
// that the second can resolve a reference to a rule written further down the
// file, or to a property whose verdict a rule reads. Both passes dispatch on
// node type rather than on header text, because each ryspec table is its own
// node type, and both track the property currently open the same way: TOML
// positions [properties.rules] after the [[properties]] it belongs to, and the
// grammar makes it a sibling rather than a child.
//
// [meta] and [extras] are skipped. They are the format's two open tables,
// facts about the document rather than part of its value space, and no name
// resolves through either.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

#define RYSPEC_MAX_BINDER_DEPTH 8
#define RYSPEC_MAX_BINDER_VARS 8

typedef struct {
  ryspec_database *db;
  ryspec_document_id document;
  const char *source;
  ryspec_name namespace_name;
  ryspec_name scope;         // the namespace, or "<namespace>.<property>"
  ryspec_symbol_id property; // the property currently open
  ryspec_symbol_id owner;    // the rule or property whose body is being lowered
  ryspec_name binders[RYSPEC_MAX_BINDER_DEPTH][RYSPEC_MAX_BINDER_VARS];
  uint32_t binder_counts[RYSPEC_MAX_BINDER_DEPTH];
  uint32_t binder_depth;
  bool overflowed;  // a quantifier nested deeper than the binder stack holds
} loader;

// -------------------------------------------------------------------- text

static const char *node_text(const loader *ld, TSNode node, size_t *length) {
  uint32_t start = ts_node_start_byte(node), end = ts_node_end_byte(node);
  *length = end - start;
  return ld->source + start;
}

// A TOML-string leaf carries its quotes in its node text: identifier,
// rule_reference, dotted_name, value_type, value_format, value_criticality and
// every operator leaf are all strings in the document. bare_key is not, so
// stripping is conditional rather than unconditional.
static ryspec_string intern_unquoted(loader *ld, TSNode node) {
  size_t length = 0;
  const char *text = node_text(ld, node, &length);
  if (length >= 2 && (text[0] == '"' || text[0] == '\'') && text[length - 1] == text[0]) {
    text++;
    length -= 2;
  }
  return ryspec_intern(ld->db, text, length);
}

// A name_reference carries braces instead: `{p}`.
static ryspec_string intern_braced(loader *ld, TSNode node) {
  size_t length = 0;
  const char *text = node_text(ld, node, &length);
  if (length >= 2 && text[0] == '{' && text[length - 1] == '}') {
    text++;
    length -= 2;
  }
  while (length > 0 && (*text == ' ' || *text == '\t')) {
    text++;
    length--;
  }
  while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t')) length--;
  return ryspec_intern(ld->db, text, length);
}

static void append_utf8(char *out, size_t *at, uint32_t code_point) {
  if (code_point < 0x80) {
    out[(*at)++] = (char)code_point;
  } else if (code_point < 0x800) {
    out[(*at)++] = (char)(0xC0 | (code_point >> 6));
    out[(*at)++] = (char)(0x80 | (code_point & 0x3F));
  } else if (code_point < 0x10000) {
    out[(*at)++] = (char)(0xE0 | (code_point >> 12));
    out[(*at)++] = (char)(0x80 | ((code_point >> 6) & 0x3F));
    out[(*at)++] = (char)(0x80 | (code_point & 0x3F));
  } else {
    out[(*at)++] = (char)(0xF0 | (code_point >> 18));
    out[(*at)++] = (char)(0x80 | ((code_point >> 12) & 0x3F));
    out[(*at)++] = (char)(0x80 | ((code_point >> 6) & 0x3F));
    out[(*at)++] = (char)(0x80 | (code_point & 0x3F));
  }
}

// A `string` or `quoted_key`, decoded to the bytes it denotes: delimiters off,
// escapes resolved, and a multi-line string's leading newline dropped. Decoding
// before interning is what makes "a\tb" and its literal spelling one entry
// rather than two.
static ryspec_string intern_decoded(loader *ld, TSNode node) {
  size_t length = 0;
  const char *text = node_text(ld, node, &length);
  if (length < 2) return ryspec_intern(ld->db, text, length);

  char quote = text[0];
  if (quote != '"' && quote != '\'') return ryspec_intern(ld->db, text, length);

  size_t run = 1;
  while (run < 3 && run < length && text[run] == quote) run++;
  size_t delimiter = run == 3 ? 3 : 1;
  if (length < delimiter * 2) return ryspec_intern(ld->db, text, length);

  text += delimiter;
  length -= delimiter * 2;
  if (delimiter == 3) {
    if (length > 0 && text[0] == '\n') {
      text++;
      length--;
    } else if (length > 1 && text[0] == '\r' && text[1] == '\n') {
      text += 2;
      length -= 2;
    }
  }

  if (quote == '\'') return ryspec_intern(ld->db, text, length);  // literal: no escapes

  // Four bytes per input byte is the worst \u can expand to.
  char stack[512];
  char *out = length * 4 + 1 < sizeof stack ? stack : ryspec_checked(malloc(length * 4 + 1));
  size_t at = 0;
  for (size_t i = 0; i < length; i++) {
    if (text[i] != '\\' || i + 1 >= length) {
      out[at++] = text[i];
      continue;
    }
    char escape = text[++i];
    switch (escape) {
      case 'b': out[at++] = '\b'; break;
      case 't': out[at++] = '\t'; break;
      case 'n': out[at++] = '\n'; break;
      case 'f': out[at++] = '\f'; break;
      case 'r': out[at++] = '\r'; break;
      case 'e': out[at++] = 0x1B; break;
      case '"': out[at++] = '"'; break;
      case '\\': out[at++] = '\\'; break;
      case 'x':
      case 'u':
      case 'U': {
        size_t digits = escape == 'x' ? 2 : (escape == 'u' ? 4 : 8);
        uint32_t code_point = 0;
        size_t taken = 0;
        while (taken < digits && i + 1 < length) {
          char digit = text[i + 1];
          uint32_t value;
          if (digit >= '0' && digit <= '9') value = (uint32_t)(digit - '0');
          else if (digit >= 'a' && digit <= 'f') value = (uint32_t)(digit - 'a') + 10;
          else if (digit >= 'A' && digit <= 'F') value = (uint32_t)(digit - 'A') + 10;
          else break;
          code_point = code_point * 16 + value;
          i++;
          taken++;
        }
        append_utf8(out, &at, code_point);
        break;
      }
      default:
        // A backslash before a newline folds the whitespace that follows it.
        if (escape == '\n' || escape == '\r' || escape == ' ' || escape == '\t') {
          while (i < length &&
                 (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) {
            i++;
          }
          i--;
        } else {
          out[at++] = '\\';
          out[at++] = escape;
        }
        break;
    }
  }

  ryspec_string handle = ryspec_intern(ld->db, out, at);
  if (out != stack) free(out);
  return handle;
}

// A key, which is a bare_key as written or a quoted_key decoded.
static ryspec_name intern_key(loader *ld, TSNode node) {
  const char *type = ts_node_type(node);
  if (strcmp(type, "quoted_key") == 0) return intern_decoded(ld, node);
  size_t length = 0;
  const char *text = node_text(ld, node, &length);
  return ryspec_intern(ld->db, text, length);
}

// ------------------------------------------------------------------ values

static ryspec_value read_value(loader *ld, TSNode node) {
  ryspec_value value = {.kind = RYSPEC_VALUE_ABSENT};
  if (ts_node_is_null(node)) return value;
  size_t length = 0;
  const char *text = node_text(ld, node, &length);
  char buffer[64];
  if (length >= sizeof buffer) length = sizeof buffer - 1;
  memcpy(buffer, text, length);
  buffer[length] = '\0';

  const char *type = ts_node_type(node);
  if (strcmp(type, "integer") == 0) {
    value.kind = RYSPEC_VALUE_INTEGER;
    value.integer = strtoll(buffer, NULL, 0);
  } else if (strcmp(type, "float") == 0) {
    value.kind = RYSPEC_VALUE_FLOAT;
    value.real = strtod(buffer, NULL);
  } else if (strcmp(type, "boolean") == 0) {
    value.kind = RYSPEC_VALUE_BOOLEAN;
    value.boolean = buffer[0] == 't';
  }
  return value;
}

static TSNode field(TSNode node, const char *name) {
  return ts_node_child_by_field_name(node, name, (uint32_t)strlen(name));
}

// --------------------------------------------------------------- operators

static ryspec_operator operator_from_text(const char *text, size_t length) {
  static const struct {
    const char *spelling;
    ryspec_operator op;
  } TABLE[] = {
      {"lt", RYSPEC_OP_LT},          {"le", RYSPEC_OP_LE},
      {"gt", RYSPEC_OP_GT},          {"ge", RYSPEC_OP_GE},
      {"eq", RYSPEC_OP_EQ},          {"ne", RYSPEC_OP_NE},
      {"<", RYSPEC_OP_LT},           {"<=", RYSPEC_OP_LE},
      {">", RYSPEC_OP_GT},           {">=", RYSPEC_OP_GE},
      {"==", RYSPEC_OP_EQ},          {"!=", RYSPEC_OP_NE},
      {"not", RYSPEC_OP_NOT},        {"prev", RYSPEC_OP_PREV},
      {"next", RYSPEC_OP_NEXT},      {"once", RYSPEC_OP_ONCE},
      {"historically", RYSPEC_OP_HISTORICALLY},
      {"eventually", RYSPEC_OP_EVENTUALLY},
      {"always", RYSPEC_OP_ALWAYS},  {"since", RYSPEC_OP_SINCE},
      {"until", RYSPEC_OP_UNTIL},    {"and", RYSPEC_OP_AND},
      {"or", RYSPEC_OP_OR},          {"xor", RYSPEC_OP_XOR},
      {"equiv", RYSPEC_OP_EQUIV},    {"implies", RYSPEC_OP_IMPLIES},
      {"->", RYSPEC_OP_IMPLIES},     {"forall", RYSPEC_OP_FORALL},
      {"exists", RYSPEC_OP_EXISTS},
  };
  for (size_t i = 0; i < sizeof TABLE / sizeof *TABLE; i++) {
    size_t spelling_length = strlen(TABLE[i].spelling);
    if (spelling_length == length && memcmp(TABLE[i].spelling, text, length) == 0) {
      return TABLE[i].op;
    }
  }
  return RYSPEC_OP_NONE;
}

// In prefix form the operator is a named node holding a TOML string; in
// expression form it is an anonymous token whose type is the literal itself
// ("and", "->", "<="). One read covers both.
static ryspec_operator read_operator(loader *ld, TSNode node) {
  if (ts_node_is_null(node)) return RYSPEC_OP_NONE;
  size_t length = 0;
  const char *text = node_text(ld, node, &length);
  if (length >= 2 && text[0] == '"' && text[length - 1] == '"') {
    text++;
    length -= 2;
  }
  return operator_from_text(text, length);
}

ryspec_cone ryspec_operator_cone(ryspec_operator op) {
  switch (op) {
    case RYSPEC_OP_PREV:
    case RYSPEC_OP_ONCE:
    case RYSPEC_OP_HISTORICALLY:
    case RYSPEC_OP_SINCE: return RYSPEC_CONE_PAST;
    case RYSPEC_OP_NEXT:
    case RYSPEC_OP_EVENTUALLY:
    case RYSPEC_OP_ALWAYS:
    case RYSPEC_OP_UNTIL: return RYSPEC_CONE_FUTURE;
    default: return RYSPEC_CONE_NEUTRAL;
  }
}

const char *ryspec_operator_name(ryspec_operator op) {
  switch (op) {
    case RYSPEC_OP_LT: return "lt";
    case RYSPEC_OP_LE: return "le";
    case RYSPEC_OP_GT: return "gt";
    case RYSPEC_OP_GE: return "ge";
    case RYSPEC_OP_EQ: return "eq";
    case RYSPEC_OP_NE: return "ne";
    case RYSPEC_OP_NOT: return "not";
    case RYSPEC_OP_PREV: return "prev";
    case RYSPEC_OP_NEXT: return "next";
    case RYSPEC_OP_ONCE: return "once";
    case RYSPEC_OP_HISTORICALLY: return "historically";
    case RYSPEC_OP_EVENTUALLY: return "eventually";
    case RYSPEC_OP_ALWAYS: return "always";
    case RYSPEC_OP_SINCE: return "since";
    case RYSPEC_OP_UNTIL: return "until";
    case RYSPEC_OP_AND: return "and";
    case RYSPEC_OP_OR: return "or";
    case RYSPEC_OP_XOR: return "xor";
    case RYSPEC_OP_EQUIV: return "equiv";
    case RYSPEC_OP_IMPLIES: return "implies";
    case RYSPEC_OP_FORALL: return "forall";
    case RYSPEC_OP_EXISTS: return "exists";
    case RYSPEC_OP_NONE: break;
  }
  return "?";
}

// ------------------------------------------------------------------ names

// A name used as a value: a predicate's operand, or a metric bound's size. It
// names a variable or a parameter, never a rule, so it is resolved but never
// lowered.
static ryspec_name value_name(loader *ld, TSNode node) {
  const char *type = ts_node_type(node);
  return strcmp(type, "name_reference") == 0 ? intern_braced(ld, node) : intern_unquoted(ld, node);
}

static bool binder_find(const loader *ld, ryspec_name name, ryspec_binder *binder) {
  for (uint32_t depth = ld->binder_depth; depth-- > 0;) {
    for (uint32_t index = 0; index < ld->binder_counts[depth]; index++) {
      if (ld->binders[depth][index] == name) {
        *binder = RYSPEC_BINDER_MAKE(ld->binder_depth - 1 - depth, index);
        return true;
      }
    }
  }
  return false;
}

// Every name a rule reads that nothing declares is an implicit input, taking a
// slot exactly as a declared one would. A quantifier-bound name is not one:
// it is local to its quantifier and names no source of value.
static ryspec_symbol_id reference_symbol(loader *ld, ryspec_name local, TSPoint point) {
  ryspec_symbol_id id = ryspec_resolve(ld->db, ld->namespace_name, ld->scope, local);
  if (id != RYSPEC_NONE) return id;
  ryspec_name qualified = ryspec_intern_qualified(ld->db, ld->namespace_name, local);
  return ryspec_symbol_declare(ld->db, qualified, local, ld->namespace_name,
                               RYSPEC_ROLE_IMPLICIT_INPUT, ld->document, point);
}

// ------------------------------------------------------------------ bounds

static void bound_entry(loader *ld, ryspec_bound *bound, TSNode node) {
  const char *type = ts_node_type(node);
  TSNode value = field(node, "value");
  if (strcmp(type, "min") == 0) {
    bound->min = read_value(ld, value);
  } else if (strcmp(type, "max") == 0) {
    bound->max = read_value(ld, value);
  } else if (strcmp(type, "bound_min") == 0) {
    bound->min_name = value_name(ld, value);
  } else if (strcmp(type, "bound_max") == 0) {
    bound->max_name = value_name(ld, value);
  }
}

// The prefix `{ min = 3, max = 10 }`. The grammar splits numeric from
// parametric by node type, so nothing here reads the text to tell them apart.
static ryspec_bound lower_bound(loader *ld, TSNode node) {
  ryspec_bound bound = {0};
  if (ts_node_is_null(node)) return bound;
  uint32_t count = ts_node_named_child_count(node);
  for (uint32_t i = 0; i < count; i++) bound_entry(ld, &bound, ts_node_named_child(node, i));
  return bound;
}

// The infix `[3:10]`, which produces the same value.
static ryspec_bound lower_metric_bound(loader *ld, TSNode node) {
  ryspec_bound bound = {0};
  if (ts_node_is_null(node)) return bound;
  TSNode min = field(node, "min"), max = field(node, "max");
  if (!ts_node_is_null(min)) {
    if (strcmp(ts_node_type(min), "name_reference") == 0) {
      bound.min_name = value_name(ld, min);
    } else {
      bound.min = read_value(ld, min);
    }
  }
  if (!ts_node_is_null(max)) {
    if (strcmp(ts_node_type(max), "name_reference") == 0) {
      bound.max_name = value_name(ld, max);
    } else {
      bound.max = read_value(ld, max);
    }
  }
  return bound;
}

// ---------------------------------------------------------------- lowering

static ryspec_rule_id lower_rule(loader *ld, TSNode node);
static ryspec_rule_id lower_expression(loader *ld, TSNode node);

static ryspec_rule_id intern(loader *ld, const ryspec_rule *term, const ryspec_rule_id *operands,
                             uint32_t count) {
  return ryspec_rule_intern(ld->db, term, operands, count);
}

// A name in rule position. Bound by an enclosing quantifier, it is a de Bruijn
// index and nothing else; otherwise it resolves, or becomes an implicit input.
static ryspec_rule_id lower_reference(loader *ld, ryspec_name local, TSPoint point) {
  ryspec_binder binder = RYSPEC_BINDER_NONE;
  if (binder_find(ld, local, &binder)) {
    ryspec_rule term = {.kind = RYSPEC_RULE_BOUND_VARIABLE, .binder = binder};
    return intern(ld, &term, NULL, 0);
  }
  ryspec_symbol_id id = reference_symbol(ld, local, point);
  ryspec_rule term = {.kind = RYSPEC_RULE_REFERENCE,
                      .name = ryspec_symbol_at(ld->db, id)->qualified};
  return intern(ld, &term, NULL, 0);
}

// One side of a comparison. Neither side is a formula, in either spelling.
static void lower_operand(loader *ld, TSNode node, ryspec_name *name, ryspec_binder *binder,
                          ryspec_value *literal) {
  const char *type = ts_node_type(node);
  if (strcmp(type, "integer") == 0 || strcmp(type, "float") == 0) {
    // An inline number is an anonymous parameter: the loader gives it a slot
    // exactly as if it had been declared, and it has no name to declare.
    *literal = read_value(ld, node);
    return;
  }
  ryspec_name local = value_name(ld, node);
  if (binder_find(ld, local, binder)) return;
  ryspec_symbol_id id = reference_symbol(ld, local, ts_node_start_point(node));
  *name = ryspec_symbol_at(ld->db, id)->qualified;
}

static ryspec_rule_id lower_predicate(loader *ld, TSNode node) {
  ryspec_rule term = {.kind = RYSPEC_RULE_PREDICATE, .op = read_operator(ld, field(node, "operator"))};
  TSNode left = field(node, "left"), right = field(node, "right");
  ryspec_value ignored = {.kind = RYSPEC_VALUE_ABSENT};
  if (!ts_node_is_null(left)) lower_operand(ld, left, &term.name, &term.left_binder, &ignored);
  if (!ts_node_is_null(right)) {
    lower_operand(ld, right, &term.right_name, &term.right_binder, &term.right_value);
  }
  return intern(ld, &term, NULL, 0);
}

// and/or/xor are associative, so a nested term of the same operator folds into
// its parent: infix `a and b and c`, which parses left-associatively, then
// meets prefix ["and", a, b, c]. implies and equiv are not associative and are
// left nested.
static bool flattens(ryspec_operator op) {
  return op == RYSPEC_OP_AND || op == RYSPEC_OP_OR || op == RYSPEC_OP_XOR;
}

static void push_operand(loader *ld, ryspec_operator op, ryspec_rule_id operand,
                         ryspec_rule_id *operands, uint32_t *count, uint32_t limit) {
  const ryspec_rule *term = ryspec_rule_at(ld->db, operand);
  if (flattens(op) && term != NULL && term->kind == RYSPEC_RULE_MULTIARY && term->op == op) {
    uint32_t inner_count = 0;
    const ryspec_rule_id *inner = ryspec_rule_operands(ld->db, operand, &inner_count);
    for (uint32_t i = 0; i < inner_count && *count < limit; i++) {
      operands[(*count)++] = inner[i];
    }
    return;
  }
  if (*count < limit) operands[(*count)++] = operand;
}

static uint32_t push_binders(loader *ld, TSNode binding_node, bool braced, ryspec_name *names,
                             uint32_t limit) {
  uint32_t count = 0;
  if (braced) {
    // quantified_expression: `variable` is a repeated field of name_reference.
    uint32_t children = ts_node_child_count(binding_node);
    for (uint32_t i = 0; i < children && count < limit; i++) {
      TSNode child = ts_node_child(binding_node, i);
      const char *name = ts_node_field_name_for_child(binding_node, i);
      if (name != NULL && strcmp(name, "variable") == 0) names[count++] = intern_braced(ld, child);
    }
    return count;
  }
  // binding: `{ vars = [...] }`.
  TSNode vars = ts_node_named_child(binding_node, 0);
  if (ts_node_is_null(vars)) return 0;
  TSNode array = field(vars, "value");
  if (ts_node_is_null(array)) return 0;
  uint32_t children = ts_node_named_child_count(array);
  for (uint32_t i = 0; i < children && count < limit; i++) {
    names[count++] = intern_unquoted(ld, ts_node_named_child(array, i));
  }
  return count;
}

// One binding record per quantifier site: the names it binds, spelled and
// placed. The term keeps only their count, so two alpha-equivalent quantifiers
// share a term while each keeps the spelling its diagnostics have to quote.
static ryspec_rule_id lower_quantified(loader *ld, TSNode node, TSNode binding_node, bool braced,
                                       TSNode operand_node, bool expression) {
  ryspec_name names[RYSPEC_MAX_BINDER_VARS];
  uint32_t count = push_binders(ld, binding_node, braced, names, RYSPEC_MAX_BINDER_VARS);

  uint32_t enclosing_first = ld->db->binding_names.count;
  for (uint32_t depth = 0; depth < ld->binder_depth; depth++) {
    for (uint32_t i = 0; i < ld->binder_counts[depth]; i++) {
      RYSPEC_PUSH(ld->db->binding_names, ld->binders[depth][i]);
    }
  }
  uint32_t enclosing_count = ld->db->binding_names.count - enclosing_first;

  uint32_t name_first = ld->db->binding_names.count;
  for (uint32_t i = 0; i < count; i++) RYSPEC_PUSH(ld->db->binding_names, names[i]);

  bool pushed = ld->binder_depth < RYSPEC_MAX_BINDER_DEPTH;
  if (pushed) {
    memcpy(ld->binders[ld->binder_depth], names, count * sizeof *names);
    ld->binder_counts[ld->binder_depth] = count;
    ld->binder_depth++;
  } else {
    ld->overflowed = true;
  }

  ryspec_rule_id operand = ts_node_is_null(operand_node)
                               ? RYSPEC_NONE
                               : (expression ? lower_expression(ld, operand_node)
                                             : lower_rule(ld, operand_node));
  if (pushed) ld->binder_depth--;

  ryspec_rule term = {.kind = RYSPEC_RULE_QUANTIFIED,
                      .op = read_operator(ld, field(node, "operator")),
                      .var_count = count};
  ryspec_rule_id id = intern(ld, &term, &operand, 1);

  ryspec_binding binding = {.rule = id,
                            .owner = ld->owner,
                            .document = ld->document,
                            .point = ts_node_start_point(node),
                            .name_first = name_first,
                            .name_count = count,
                            .enclosing_first = enclosing_first,
                            .enclosing_count = enclosing_count,
                            .scope = ld->scope};
  RYSPEC_PUSH(ld->db->bindings, binding);
  return id;
}

// _rule: the prefix spelling, plus `expression`, which is the infix one.
static ryspec_rule_id lower_rule(loader *ld, TSNode node) {
  if (ts_node_is_null(node)) return RYSPEC_NONE;
  const char *type = ts_node_type(node);

  if (strcmp(type, "rule_reference") == 0) {
    return lower_reference(ld, intern_unquoted(ld, node), ts_node_start_point(node));
  }
  if (strcmp(type, "expression") == 0) {
    return lower_expression(ld, ts_node_named_child(node, 0));
  }
  if (strcmp(type, "predicate_rule") == 0) return lower_predicate(ld, node);

  if (strcmp(type, "unary_rule") == 0) {
    ryspec_rule_id operand = lower_rule(ld, field(node, "operand"));
    ryspec_rule term = {.kind = RYSPEC_RULE_UNARY, .op = read_operator(ld, field(node, "operator"))};
    return intern(ld, &term, &operand, 1);
  }
  if (strcmp(type, "unary_temporal_rule") == 0) {
    ryspec_rule_id operand = lower_rule(ld, field(node, "operand"));
    ryspec_rule term = {.kind = RYSPEC_RULE_UNARY_TEMPORAL,
                        .op = read_operator(ld, field(node, "operator")),
                        .bound = lower_bound(ld, field(node, "bound"))};
    return intern(ld, &term, &operand, 1);
  }
  if (strcmp(type, "binary_temporal_rule") == 0) {
    ryspec_rule_id operands[2] = {lower_rule(ld, field(node, "left")),
                                  lower_rule(ld, field(node, "right"))};
    ryspec_rule term = {.kind = RYSPEC_RULE_BINARY_TEMPORAL,
                        .op = read_operator(ld, field(node, "operator")),
                        .bound = lower_bound(ld, field(node, "bound"))};
    return intern(ld, &term, operands, 2);
  }
  if (strcmp(type, "multiary_rule") == 0) {
    ryspec_operator op = read_operator(ld, field(node, "operator"));
    ryspec_rule_id operands[64];
    uint32_t count = 0;
    uint32_t children = ts_node_child_count(node);
    for (uint32_t i = 0; i < children; i++) {
      const char *name = ts_node_field_name_for_child(node, i);
      if (name == NULL || strcmp(name, "operand") != 0) continue;
      push_operand(ld, op, lower_rule(ld, ts_node_child(node, i)), operands, &count, 64);
    }
    ryspec_rule term = {.kind = RYSPEC_RULE_MULTIARY, .op = op};
    return intern(ld, &term, operands, count);
  }
  if (strcmp(type, "quantified_rule") == 0) {
    return lower_quantified(ld, node, field(node, "binding"), false, field(node, "operand"), false);
  }
  return RYSPEC_NONE;
}

// _expression: the infix spelling, onto the same terms. A parenthesised
// expression is transparent, which is itself a small win -- "(({p}))" and
// ["not"...]'s operand meet without it.
static ryspec_rule_id lower_expression(loader *ld, TSNode node) {
  if (ts_node_is_null(node)) return RYSPEC_NONE;
  const char *type = ts_node_type(node);

  if (strcmp(type, "parenthesized_expression") == 0) {
    return lower_expression(ld, ts_node_named_child(node, 0));
  }
  if (strcmp(type, "name_reference") == 0) {
    return lower_reference(ld, intern_braced(ld, node), ts_node_start_point(node));
  }
  if (strcmp(type, "comparison_expression") == 0) return lower_predicate(ld, node);

  if (strcmp(type, "unary_expression") == 0) {
    ryspec_rule_id operand = lower_expression(ld, field(node, "operand"));
    ryspec_rule term = {.kind = RYSPEC_RULE_UNARY, .op = read_operator(ld, field(node, "operator"))};
    return intern(ld, &term, &operand, 1);
  }
  if (strcmp(type, "unary_temporal_expression") == 0) {
    ryspec_rule_id operand = lower_expression(ld, field(node, "operand"));
    ryspec_rule term = {.kind = RYSPEC_RULE_UNARY_TEMPORAL,
                        .op = read_operator(ld, field(node, "operator")),
                        .bound = lower_metric_bound(ld, field(node, "bound"))};
    return intern(ld, &term, &operand, 1);
  }
  if (strcmp(type, "binary_temporal_expression") == 0) {
    ryspec_rule_id operands[2] = {lower_expression(ld, field(node, "left")),
                                  lower_expression(ld, field(node, "right"))};
    ryspec_rule term = {.kind = RYSPEC_RULE_BINARY_TEMPORAL,
                        .op = read_operator(ld, field(node, "operator")),
                        .bound = lower_metric_bound(ld, field(node, "bound"))};
    return intern(ld, &term, operands, 2);
  }
  if (strcmp(type, "binary_expression") == 0) {
    ryspec_operator op = read_operator(ld, field(node, "operator"));
    ryspec_rule_id operands[64];
    uint32_t count = 0;
    push_operand(ld, op, lower_expression(ld, field(node, "left")), operands, &count, 64);
    push_operand(ld, op, lower_expression(ld, field(node, "right")), operands, &count, 64);
    ryspec_rule term = {.kind = RYSPEC_RULE_MULTIARY, .op = op};
    return intern(ld, &term, operands, count);
  }
  if (strcmp(type, "quantified_expression") == 0) {
    return lower_quantified(ld, node, node, true, field(node, "operand"), true);
  }
  return RYSPEC_NONE;
}

// ----------------------------------------------------------- declarations

static void declare_list(loader *ld, TSNode list, uint32_t role) {
  TSNode array = field(list, "value");
  if (ts_node_is_null(array)) return;
  uint32_t count = ts_node_named_child_count(array);
  for (uint32_t i = 0; i < count; i++) {
    TSNode entry = ts_node_named_child(array, i);
    ryspec_name local = intern_unquoted(ld, entry);
    // A dotted `outputs` entry reaches a rule private to a property, and its
    // qualified key is the namespace in front of what it already spells -- the
    // same key that private rule was declared under.
    ryspec_name qualified = ryspec_intern_qualified(ld->db, ld->namespace_name, local);
    ryspec_symbol_id id = ryspec_symbol_declare(ld->db, qualified, local, ld->namespace_name, role,
                                                ld->document, ts_node_start_point(entry));
    ryspec_symbol *symbol = ryspec_symbol_mut(ld->db, id);
    if (symbol->partition_order == 0) symbol->partition_order = i + 1;
  }
}

static void declare_variable_attribute(loader *ld, ryspec_symbol_id id, TSNode node) {
  ryspec_symbol *symbol = ryspec_symbol_mut(ld->db, id);
  const char *type = ts_node_type(node);
  TSNode value = field(node, "value");
  if (strcmp(type, "type") == 0) {
    size_t length = 0;
    ryspec_string handle = intern_unquoted(ld, value);
    const char *text = ryspec_text(ld->db, handle, &length);
    symbol->has_type = true;
    if (strcmp(text, "bool") == 0) symbol->type = RYSPEC_TYPE_BOOL;
    else if (strcmp(text, "text") == 0) symbol->type = RYSPEC_TYPE_TEXT;
    else if (strcmp(text, "binary") == 0) symbol->type = RYSPEC_TYPE_BINARY;
    else symbol->type = RYSPEC_TYPE_NUMBER;
  } else if (strcmp(type, "unit") == 0) {
    symbol->unit = intern_decoded(ld, value);
  } else if (strcmp(type, "title") == 0) {
    symbol->title = intern_decoded(ld, value);
  } else if (strcmp(type, "description") == 0) {
    symbol->description = intern_decoded(ld, value);
  } else if (strcmp(type, "source") == 0) {
    symbol->source = intern_unquoted(ld, value);
    symbol->role_points[ryspec_role_slot(RYSPEC_ROLE_VARIABLE)] = ts_node_start_point(node);
  } else if (strcmp(type, "format") == 0) {
    symbol->format = intern_unquoted(ld, value);
  } else if (strcmp(type, "initial_value") == 0) {
    symbol->initial_value = read_value(ld, value);
  } else if (strcmp(type, "min") == 0) {
    symbol->min = read_value(ld, value);
  } else if (strcmp(type, "max") == 0) {
    symbol->max = read_value(ld, value);
  }
}

static ryspec_symbol_id declare_variable(loader *ld, TSNode name_node, TSNode attributes) {
  ryspec_name local = intern_key(ld, name_node);
  ryspec_name qualified = ryspec_intern_qualified(ld->db, ld->namespace_name, local);
  ryspec_symbol_id id =
      ryspec_symbol_declare(ld->db, qualified, local, ld->namespace_name, RYSPEC_ROLE_VARIABLE,
                            ld->document, ts_node_start_point(name_node));
  uint32_t count = ts_node_named_child_count(attributes);
  for (uint32_t i = 0; i < count; i++) {
    declare_variable_attribute(ld, id, ts_node_named_child(attributes, i));
  }
  return id;
}

// A rule's name, declared before any body is lowered so that a rule written
// above the one it reads still resolves.
static ryspec_symbol_id declare_rule_name(loader *ld, TSNode definition) {
  TSNode name_node = field(definition, "name");
  if (ts_node_is_null(name_node)) return RYSPEC_NONE;
  ryspec_name local = intern_key(ld, name_node);
  ryspec_name qualified = ryspec_intern_qualified(ld->db, ld->scope, local);
  return ryspec_symbol_declare(ld->db, qualified, local, ld->scope, RYSPEC_ROLE_RULE, ld->document,
                               ts_node_start_point(name_node));
}

// The scope a property's private rules live in: "<namespace>.<property>", so
// two properties may each define `rhs` without colliding and a qualified
// output reaches one of them by the name it already spells.
static void open_property(loader *ld, TSNode property, bool declare) {
  ld->property = RYSPEC_NONE;
  ld->scope = ld->namespace_name;

  uint32_t count = ts_node_named_child_count(property);
  for (uint32_t i = 0; i < count; i++) {
    TSNode child = ts_node_named_child(property, i);
    if (strcmp(ts_node_type(child), "name") != 0) continue;
    TSNode value = field(child, "value");
    if (ts_node_is_null(value)) continue;
    ryspec_name local = intern_unquoted(ld, value);
    ryspec_name qualified = ryspec_intern_qualified(ld->db, ld->namespace_name, local);
    if (declare) {
      // Two [[properties]] entries sharing a name merge into one symbol, one
      // namespace meaning one record per name, so the second declaration is
      // the only place the fact still exists. Hence the check is here rather
      // than in semantics.c with the other eleven.
      ryspec_symbol_id existing = ryspec_lookup(ld->db, qualified);
      const ryspec_symbol *before = ryspec_symbol_at(ld->db, existing);
      if (before != NULL && (before->roles & RYSPEC_ROLE_PROPERTY)) {
        TSPoint first = before->role_points[ryspec_role_slot(RYSPEC_ROLE_PROPERTY)];
        // Two documents in one namespace can each declare it, so the earlier
        // place is named by file where that is not this one.
        char where[512];
        if (before->document == ld->document) {
          snprintf(where, sizeof where, "line %u, column %u", first.row + 1, first.column + 1);
        } else {
          const ryspec_document *other = ryspec_document_at(ld->db, before->document);
          snprintf(where, sizeof where, "%s:%u:%u", ryspec_text(ld->db, other->path, NULL),
                   first.row + 1, first.column + 1);
        }
        ryspec_report(ld->db, ld->document, ts_node_start_point(value),
                      "duplicate property name '%s' (also declared at %s)",
                      ryspec_text(ld->db, local, NULL), where);
      }
      ld->property = ryspec_symbol_declare(ld->db, qualified, local, ld->namespace_name,
                                           RYSPEC_ROLE_PROPERTY, ld->document,
                                           ts_node_start_point(value));
    } else {
      ld->property = ryspec_lookup(ld->db, qualified);
    }
    ld->scope = qualified;
    return;
  }
}

// -------------------------------------------------------------- the passes

static void declare_pass(loader *ld, TSNode root) {
  uint32_t count = ts_node_named_child_count(root);
  for (uint32_t i = 0; i < count; i++) {
    TSNode child = ts_node_named_child(root, i);
    const char *type = ts_node_type(child);

    if (strcmp(type, "namespace") == 0) {
      ld->namespace_name = intern_unquoted(ld, field(child, "value"));
      ld->scope = ld->namespace_name;
      ryspec_document *document = &ld->db->documents.items[ld->document];
      document->namespace_name = ld->namespace_name;
    } else if (strcmp(type, "version") == 0) {
      ryspec_value value = read_value(ld, field(child, "value"));
      ryspec_document *document = &ld->db->documents.items[ld->document];
      document->version = value.kind == RYSPEC_VALUE_INTEGER ? value.integer : 0;
      document->has_version = value.kind == RYSPEC_VALUE_INTEGER;
    } else if (strcmp(type, "monitor_table") == 0) {
      uint32_t entries = ts_node_named_child_count(child);
      for (uint32_t j = 0; j < entries; j++) {
        TSNode entry = ts_node_named_child(child, j);
        const char *list = ts_node_type(entry);
        uint32_t role = 0;
        if (strcmp(list, "inputs") == 0) role = RYSPEC_ROLE_INPUT;
        else if (strcmp(list, "outputs") == 0) role = RYSPEC_ROLE_OUTPUT;
        else if (strcmp(list, "parameters") == 0) role = RYSPEC_ROLE_PARAMETER;
        if (role == 0) continue;
        // The list is recorded as written even when it is empty, which no
        // symbol could record: `parameters = []` says there are none, and
        // omitting it asks the loader to deduce them.
        ld->db->documents.items[ld->document].monitor_lists |= role;
        declare_list(ld, entry, role);
      }
    } else if (strcmp(type, "variables_table") == 0) {
      uint32_t entries = ts_node_named_child_count(child);
      for (uint32_t j = 0; j < entries; j++) {
        TSNode entry = ts_node_named_child(child, j);
        declare_variable(ld, field(entry, "name"), field(entry, "value"));
      }
    } else if (strcmp(type, "variable_table") == 0) {
      declare_variable(ld, field(child, "name"), child);
    } else if (strcmp(type, "rules_table") == 0) {
      ld->scope = ld->namespace_name;
      uint32_t entries = ts_node_named_child_count(child);
      for (uint32_t j = 0; j < entries; j++) declare_rule_name(ld, ts_node_named_child(child, j));
    } else if (strcmp(type, "property") == 0) {
      open_property(ld, child, true);
      // `rules = { ... }` written inline in the property rather than as a
      // [properties.rules] table of its own.
      uint32_t entries = ts_node_named_child_count(child);
      for (uint32_t j = 0; j < entries; j++) {
        TSNode entry = ts_node_named_child(child, j);
        if (strcmp(ts_node_type(entry), "rules") != 0) continue;
        TSNode table = field(entry, "value");
        uint32_t definitions = ts_node_named_child_count(table);
        for (uint32_t k = 0; k < definitions; k++) {
          declare_rule_name(ld, ts_node_named_child(table, k));
        }
      }
    } else if (strcmp(type, "property_rules_table") == 0) {
      uint32_t entries = ts_node_named_child_count(child);
      for (uint32_t j = 0; j < entries; j++) declare_rule_name(ld, ts_node_named_child(child, j));
    }
    // meta_table, extras_table, features_table, monitor_runtime_table, table,
    // table_array_element and a root pair declare no name.
  }
}

static void lower_definition(loader *ld, TSNode definition) {
  TSNode name_node = field(definition, "name");
  if (ts_node_is_null(name_node)) return;
  ryspec_name local = intern_key(ld, name_node);
  ryspec_name qualified = ryspec_intern_qualified(ld->db, ld->scope, local);
  ryspec_symbol_id id = ryspec_lookup(ld->db, qualified);
  if (id == RYSPEC_NONE) return;

  ld->owner = id;
  TSNode value = field(definition, "value");
  ryspec_symbol_mut(ld->db, id)->rule = lower_rule(ld, value);
  ld->owner = RYSPEC_NONE;
}

static void lower_property(loader *ld, TSNode property) {
  open_property(ld, property, false);
  ryspec_symbol_id id = ld->property;
  ld->owner = id;

  uint32_t count = ts_node_named_child_count(property);
  for (uint32_t i = 0; i < count; i++) {
    TSNode child = ts_node_named_child(property, i);
    const char *type = ts_node_type(child);
    TSNode value = field(child, "value");

    if (strcmp(type, "given") == 0 || strcmp(type, "check") == 0 || strcmp(type, "impose") == 0) {
      ryspec_rule_id rule = lower_rule(ld, value);
      ryspec_symbol *symbol = ryspec_symbol_mut(ld->db, id);
      if (symbol != NULL) {
        if (type[0] == 'g') symbol->given = rule;
        else if (type[0] == 'c') symbol->check = rule;
        else symbol->impose = rule;
      }
    } else if (strcmp(type, "criticality") == 0) {
      size_t length = 0;
      const char *text = ryspec_text(ld->db, intern_unquoted(ld, value), &length);
      ryspec_symbol *symbol = ryspec_symbol_mut(ld->db, id);
      if (strcmp(text, "info") == 0) symbol->criticality = RYSPEC_CRITICALITY_INFO;
      else if (strcmp(text, "warning") == 0) symbol->criticality = RYSPEC_CRITICALITY_WARNING;
      else if (strcmp(text, "error") == 0) symbol->criticality = RYSPEC_CRITICALITY_ERROR;
      else if (strcmp(text, "critical") == 0) symbol->criticality = RYSPEC_CRITICALITY_CRITICAL;
    } else if (strcmp(type, "message") == 0) {
      ryspec_symbol_mut(ld->db, id)->message = intern_decoded(ld, value);
    } else if (strcmp(type, "title") == 0) {
      ryspec_symbol_mut(ld->db, id)->title = intern_decoded(ld, value);
    } else if (strcmp(type, "description") == 0) {
      ryspec_symbol_mut(ld->db, id)->description = intern_decoded(ld, value);
    } else if (strcmp(type, "rules") == 0) {
      uint32_t definitions = ts_node_named_child_count(value);
      for (uint32_t j = 0; j < definitions; j++) {
        lower_definition(ld, ts_node_named_child(value, j));
      }
      ld->owner = id;
    }
  }
  ld->owner = RYSPEC_NONE;
}

static void lower_pass(loader *ld, TSNode root) {
  ld->scope = ld->namespace_name;
  uint32_t count = ts_node_named_child_count(root);
  for (uint32_t i = 0; i < count; i++) {
    TSNode child = ts_node_named_child(root, i);
    const char *type = ts_node_type(child);

    if (strcmp(type, "rules_table") == 0) {
      ld->scope = ld->namespace_name;
      uint32_t entries = ts_node_named_child_count(child);
      for (uint32_t j = 0; j < entries; j++) lower_definition(ld, ts_node_named_child(child, j));
    } else if (strcmp(type, "property") == 0) {
      lower_property(ld, child);
    } else if (strcmp(type, "property_rules_table") == 0) {
      uint32_t entries = ts_node_named_child_count(child);
      for (uint32_t j = 0; j < entries; j++) lower_definition(ld, ts_node_named_child(child, j));
    }
  }
}

void ryspec_load_tree(ryspec_database *db, ryspec_document_id document, TSNode root,
                      const char *source) {
  loader ld = {.db = db, .document = document, .source = source};
  declare_pass(&ld, root);
  ld.property = RYSPEC_NONE;
  lower_pass(&ld, root);
  if (ld.overflowed) {
    ryspec_report(db, document, ts_node_start_point(root),
                  "quantifiers nested more than %d deep are not supported",
                  RYSPEC_MAX_BINDER_DEPTH);
  }
}
