/*
 * The expression parser, private to libryspec: an expression-form string
 * walked, by recursive descent, into a rule's events, which are written as
 * prefix-form TOML.
 *
 * The parser emits a rule as a stream of events, an operator's ENTER, its
 * operands, the names a quantifier binds, then its LEAVE: prefix form's
 * order. A sink passes each to a function until it returns false, and drops
 * every event after. ryspec_rule_format_event() is such a function, writing the
 * rule in prefix form as TOML, which is how an expression becomes a
 * document's value (translate.c).
 */
#ifndef RYSPEC_SRC_EXPR_PARSER_H
#define RYSPEC_SRC_EXPR_PARSER_H

#include <stdbool.h>
#include <stddef.h>

#include "ryspec/ryspec.h"

/* The operators, in the order of SPEC.md's "Rules in prefix form". */
typedef enum ryspec_rule_op {
  RYSPEC_RULE_OP_NOT,
  RYSPEC_RULE_OP_AND,
  RYSPEC_RULE_OP_OR,
  RYSPEC_RULE_OP_XOR,
  RYSPEC_RULE_OP_EQUIV,
  RYSPEC_RULE_OP_IMPLIES,
  RYSPEC_RULE_OP_PREV,
  RYSPEC_RULE_OP_NEXT,
  RYSPEC_RULE_OP_ONCE,
  RYSPEC_RULE_OP_HISTORICALLY,
  RYSPEC_RULE_OP_EVENTUALLY,
  RYSPEC_RULE_OP_ALWAYS,
  RYSPEC_RULE_OP_SINCE,
  RYSPEC_RULE_OP_UNTIL,
  RYSPEC_RULE_OP_LT,
  RYSPEC_RULE_OP_LE,
  RYSPEC_RULE_OP_GT,
  RYSPEC_RULE_OP_GE,
  RYSPEC_RULE_OP_EQ,
  RYSPEC_RULE_OP_NE,
  RYSPEC_RULE_OP_CONTAINS,
  RYSPEC_RULE_OP_STARTSWITH,
  RYSPEC_RULE_OP_ENDSWITH,
  RYSPEC_RULE_OP_ASSIGN,
  RYSPEC_RULE_OP_FORALL,
  RYSPEC_RULE_OP_EXISTS,
} ryspec_rule_op;

/* The operator's prefix spelling, as "once". */
const char *ryspec_rule_op_name(ryspec_rule_op op);

/* The operator spelled name in prefix form, in *out, returning whether there
 * is one. */
bool ryspec_rule_op_from_name(const char *name, ryspec_rule_op *out);

/* len bytes at ptr, not NUL-terminated: a slice of the expression. */
typedef struct ryspec_rule_slice {
  const char *ptr;
  size_t len;
} ryspec_rule_slice;

/* A metric bound. An end is a number, or the name of a parameter when its
 * *_name.ptr is not NULL. */
typedef struct ryspec_rule_bound {
  bool has_min, has_max;
  double min, max;
  ryspec_rule_slice min_name, max_name;
  const char *time_unit; /* "ns", "us", "ms", "s", "min", "h", "d", or NULL */
} ryspec_rule_bound;

typedef enum ryspec_rule_event_kind {
  RYSPEC_RULE_ENTER,     /* an operator, before its operands: op, bound */
  RYSPEC_RULE_LEAVE,     /* the operator ENTER began: op and bound again */
  RYSPEC_RULE_REFERENCE, /* a name: text */
  RYSPEC_RULE_QVAR,      /* { qvar = "a" }, assign's third operand: text */
  RYSPEC_RULE_BIND,      /* a name forall or exists binds, after its
                          * operand: text */
  RYSPEC_RULE_NUMBER,    /* { value = 30 }: number */
} ryspec_rule_event_kind;

typedef struct ryspec_rule_event {
  ryspec_rule_event_kind kind;
  ryspec_rule_op op;
  bool has_bound;
  ryspec_rule_bound bound;
  ryspec_rule_slice text;
  double number;
  int line, column; /* 1-based, of its first token in the expression */
} ryspec_rule_event;

/* Receives one event, returning false to end the walk. */
typedef bool (*ryspec_rule_fn)(const ryspec_rule_event *event, void *ctx);

typedef struct ryspec_rule_sink {
  ryspec_rule_fn fn; /* may be NULL */
  void *ctx;
  bool stopped; /* fn returned false */
} ryspec_rule_sink;

/* Pass event to sink, which may be NULL. */
void ryspec_rule_emit(ryspec_rule_sink *sink, const ryspec_rule_event *event);

/* An operator's ENTER and LEAVE, with its bound, which may be NULL. */
void ryspec_rule_enter(ryspec_rule_sink *sink, ryspec_rule_op op,
                       const ryspec_rule_bound *bound, int line, int column);
void ryspec_rule_leave(ryspec_rule_sink *sink, ryspec_rule_op op,
                       const ryspec_rule_bound *bound, int line, int column);

/* A leaf holding len bytes of s: a REFERENCE, QVAR or BIND. */
void ryspec_rule_leaf(ryspec_rule_sink *sink, ryspec_rule_event_kind kind,
                      const char *s, size_t len, int line, int column);

void ryspec_rule_number(ryspec_rule_sink *sink, double value, int line,
                        int column);

/* Set one end of bound: a number, or, when name is not NULL, the parameter
 * of name_len bytes. */
void ryspec_rule_bound_end(ryspec_rule_bound *bound, bool max, double value,
                           const char *name, size_t name_len);

/* The library's spelling of the bound unit of len bytes at name, or NULL
 * when it is not one. */
const char *ryspec_rule_unit(const char *name, size_t len);

/* ---------------------------------------------------------------------------
 * TOML text, written into buf of size bytes and cut to fit, as snprintf
 * does: len counts what the whole would take. buf may be NULL when size is
 * 0, to measure. */

typedef struct ryspec_rule_out {
  char *buf;
  size_t size, len;
} ryspec_rule_out;

/* Terminate what out holds. */
void ryspec_rule_out_finish(ryspec_rule_out *out);

void ryspec_rule_put_bytes(ryspec_rule_out *out, const char *s, size_t len);

/* s as a TOML basic string, quoted and escaped. */
void ryspec_rule_put_string(ryspec_rule_out *out, const char *s, size_t len);

/* x as a TOML number: an integer when it is one exactly, else the shortest
 * float that reads back as x. */
void ryspec_rule_put_number(ryspec_rule_out *out, double x);

/* Writes a rule's events as one prefix-form TOML value. */
typedef struct ryspec_rule_formatter {
  ryspec_rule_out out;
  bool sep;   /* an item is written, so the next takes a comma */
  bool binds; /* a quantifier's names are being written */
} ryspec_rule_formatter;

/* A ryspec_rule_fn writing each event into the ryspec_rule_formatter ctx. */
bool ryspec_rule_format_event(const ryspec_rule_event *event, void *formatter);

/* ---------------------------------------------------------------------------
 * The parser. */

/* Check len bytes of src, then, when it parses, emit its events to sink,
 * which may be NULL to check alone. Returns whether src parses; a string
 * that does not parse emits nothing. diag, which may be NULL, is filled
 * with RYSPEC_OK, or RYSPEC_ERROR_GRAMMAR placed at the fault in src, not
 * in the document: line and column count from src's first byte. */
bool ryspec_expr_walk(const char *src, size_t len, ryspec_rule_sink *sink,
                      ryspec_diagnostic *diag);

#endif /* RYSPEC_SRC_EXPR_PARSER_H */
