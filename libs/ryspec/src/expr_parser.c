/*
 * The expression parser of expr_parser.h: the operators, the events and their
 * sink, and the writers of TOML text; then expression form parsed by
 * recursive descent, one function per production of SPEC.md's grammar,
 * into those events.
 *
 * A string is parsed twice: once with no sink, to check it, and, when it
 * parses, once more to emit. Events come operator first, so a level whose
 * operators gather on the left -- `and`, `or` and `xor`, `since` and
 * `until` -- looks ahead before its first operand for the operators it
 * will meet (runs()), and opens them all, the outermost first. Nothing is
 * kept: a name is a slice of the string, and a lookahead a copy of the
 * parser, scanned and dropped.
 *
 * A token is lexed as the grammar has one where it stands. A word is a
 * keyword only where the grammar has one: `{once}` names a
 * variable `once`, `[:s]` bounds by a parameter `s`, and only the third field
 * of a bound reads `s` as seconds. Inside a bound, which has no `:=`, a `:`
 * never joins the `=` after it.
 */
#include "expr_parser.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const op_names[] = {
    [RYSPEC_RULE_OP_NOT] = "not",
    [RYSPEC_RULE_OP_AND] = "and",
    [RYSPEC_RULE_OP_OR] = "or",
    [RYSPEC_RULE_OP_XOR] = "xor",
    [RYSPEC_RULE_OP_EQUIV] = "equiv",
    [RYSPEC_RULE_OP_IMPLIES] = "implies",
    [RYSPEC_RULE_OP_PREV] = "prev",
    [RYSPEC_RULE_OP_NEXT] = "next",
    [RYSPEC_RULE_OP_ONCE] = "once",
    [RYSPEC_RULE_OP_HISTORICALLY] = "historically",
    [RYSPEC_RULE_OP_EVENTUALLY] = "eventually",
    [RYSPEC_RULE_OP_ALWAYS] = "always",
    [RYSPEC_RULE_OP_SINCE] = "since",
    [RYSPEC_RULE_OP_UNTIL] = "until",
    [RYSPEC_RULE_OP_LT] = "lt",
    [RYSPEC_RULE_OP_LE] = "le",
    [RYSPEC_RULE_OP_GT] = "gt",
    [RYSPEC_RULE_OP_GE] = "ge",
    [RYSPEC_RULE_OP_EQ] = "eq",
    [RYSPEC_RULE_OP_NE] = "ne",
    [RYSPEC_RULE_OP_CONTAINS] = "contains",
    [RYSPEC_RULE_OP_STARTSWITH] = "startswith",
    [RYSPEC_RULE_OP_ENDSWITH] = "endswith",
    [RYSPEC_RULE_OP_ASSIGN] = "assign",
    [RYSPEC_RULE_OP_FORALL] = "forall",
    [RYSPEC_RULE_OP_EXISTS] = "exists",
};

#define OP_COUNT (sizeof op_names / sizeof op_names[0])

static const char *const units[] = {"ns", "us", "ms", "s", "min", "h", "d"};

/* ---------------------------------------------------------------------------
 * Events. */

void ryspec_rule_emit(ryspec_rule_sink *sink, const ryspec_rule_event *event) {
  if (sink && !sink->stopped && sink->fn && !sink->fn(event, sink->ctx)) {
    sink->stopped = true;
  }
}

static void operator(ryspec_rule_sink *sink, ryspec_rule_event_kind kind,
                     ryspec_rule_op op, const ryspec_rule_bound *bound,
                     int line, int column) {
  ryspec_rule_event event = {
      .kind = kind, .op = op, .line = line, .column = column};
  if (bound) {
    event.has_bound = true;
    event.bound = *bound;
  }
  ryspec_rule_emit(sink, &event);
}

void ryspec_rule_enter(ryspec_rule_sink *sink, ryspec_rule_op op,
                       const ryspec_rule_bound *bound, int line, int column) {
  operator(sink, RYSPEC_RULE_ENTER, op, bound, line, column);
}

void ryspec_rule_leave(ryspec_rule_sink *sink, ryspec_rule_op op,
                       const ryspec_rule_bound *bound, int line, int column) {
  operator(sink, RYSPEC_RULE_LEAVE, op, bound, line, column);
}

void ryspec_rule_leaf(ryspec_rule_sink *sink, ryspec_rule_event_kind kind,
                      const char *s, size_t len, int line, int column) {
  ryspec_rule_emit(sink, &(ryspec_rule_event){.kind = kind,
                                              .text = {s, len},
                                              .line = line,
                                              .column = column});
}

void ryspec_rule_number(ryspec_rule_sink *sink, double value, int line,
                        int column) {
  ryspec_rule_emit(sink, &(ryspec_rule_event){.kind = RYSPEC_RULE_NUMBER,
                                              .number = value,
                                              .line = line,
                                              .column = column});
}

void ryspec_rule_bound_end(ryspec_rule_bound *bound, bool max, double value,
                           const char *name, size_t name_len) {
  ryspec_rule_slice s = {name, name ? name_len : 0};
  if (max) {
    bound->has_max = true;
    bound->max = name ? 0 : value;
    bound->max_name = s;
  } else {
    bound->has_min = true;
    bound->min = name ? 0 : value;
    bound->min_name = s;
  }
}

const char *ryspec_rule_unit(const char *name, size_t len) {
  for (size_t i = 0; i < sizeof units / sizeof units[0]; i++) {
    if (strlen(units[i]) == len && memcmp(units[i], name, len) == 0) {
      return units[i];
    }
  }
  return NULL;
}

/* ---------------------------------------------------------------------------
 * Operators. */

const char *ryspec_rule_op_name(ryspec_rule_op op) {
  return (size_t)op < OP_COUNT ? op_names[op] : NULL;
}

bool ryspec_rule_op_from_name(const char *name, ryspec_rule_op *out) {
  for (size_t i = 0; i < OP_COUNT; i++) {
    if (strcmp(op_names[i], name) == 0) {
      *out = (ryspec_rule_op)i;
      return true;
    }
  }
  return false;
}

/* ---------------------------------------------------------------------------
 * TOML text. */

void ryspec_rule_out_finish(ryspec_rule_out *out) {
  if (out->size) {
    out->buf[out->len < out->size ? out->len : out->size - 1] = '\0';
  }
}

void ryspec_rule_put_bytes(ryspec_rule_out *out, const char *s, size_t len) {
  for (size_t i = 0; i < len; i++, out->len++) {
    if (out->len + 1 < out->size) {
      out->buf[out->len] = s[i];
    }
  }
}

static void put(ryspec_rule_out *out, const char *s) {
  ryspec_rule_put_bytes(out, s, strlen(s));
}

void ryspec_rule_put_string(ryspec_rule_out *out, const char *s, size_t len) {
  put(out, "\"");
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    char tmp[8];
    if (c == '"' || c == '\\') {
      snprintf(tmp, sizeof tmp, "\\%c", c);
    } else if (c < 0x20 || c == 0x7f) {
      snprintf(tmp, sizeof tmp, "\\u%04X", c);
    } else {
      tmp[0] = (char)c;
      tmp[1] = '\0';
    }
    put(out, tmp);
  }
  put(out, "\"");
}

void ryspec_rule_put_number(ryspec_rule_out *out, double x) {
  char tmp[40];
  if (isinf(x)) {
    put(out, x < 0 ? "-inf" : "inf");
    return;
  }
  if (isnan(x)) {
    put(out, "nan");
    return;
  }
  if (x > -1e15 && x < 1e15 && x == (double)(long long)x) {
    snprintf(tmp, sizeof tmp, "%.0f", x);
    put(out, tmp);
    return;
  }
  /* The shortest spelling that reads back as x. */
  for (int p = 1; p <= 17; p++) {
    snprintf(tmp, sizeof tmp, "%.*g", p, x);
    if (strtod(tmp, NULL) == x) {
      break;
    }
  }
  put(out, tmp);
  if (!strpbrk(tmp, ".eE")) {
    put(out, ".0");
  }
}

static void put_end(ryspec_rule_out *o, const char *key, double x,
                    ryspec_rule_slice name, bool *first) {
  put(o, *first ? " " : ", ");
  put(o, key);
  put(o, " = ");
  *first = false;
  if (name.ptr) {
    ryspec_rule_put_string(o, name.ptr, name.len);
  } else {
    ryspec_rule_put_number(o, x);
  }
}

static void put_bound(ryspec_rule_out *o, const ryspec_rule_bound *b) {
  bool first = true;
  put(o, ", {");
  if (b->has_min) {
    put_end(o, "min", b->min, b->min_name, &first);
  }
  if (b->has_max) {
    put_end(o, "max", b->max, b->max_name, &first);
  }
  if (b->time_unit) {
    put(o, first ? " time_unit = " : ", time_unit = ");
    ryspec_rule_put_string(o, b->time_unit, strlen(b->time_unit));
  }
  put(o, " }");
}

bool ryspec_rule_format_event(const ryspec_rule_event *e, void *formatter) {
  ryspec_rule_formatter *f = formatter;
  ryspec_rule_out *o = &f->out;
  if (e->kind == RYSPEC_RULE_BIND) {
    put(o, f->binds ? ", " : ", { qvars = [");
    f->binds = true;
    ryspec_rule_put_string(o, e->text.ptr, e->text.len);
  } else if (e->kind == RYSPEC_RULE_LEAVE) {
    if (f->binds) {
      put(o, "] }");
      f->binds = false;
    }
    if (e->has_bound) {
      put_bound(o, &e->bound);
    }
    put(o, "]");
  } else {
    if (f->sep) {
      put(o, ", ");
    }
    switch (e->kind) {
    case RYSPEC_RULE_ENTER:
      put(o, "[");
      ryspec_rule_put_string(o, op_names[e->op], strlen(op_names[e->op]));
      break;
    case RYSPEC_RULE_REFERENCE:
      ryspec_rule_put_string(o, e->text.ptr, e->text.len);
      break;
    case RYSPEC_RULE_QVAR:
      put(o, "{ qvar = ");
      ryspec_rule_put_string(o, e->text.ptr, e->text.len);
      put(o, " }");
      break;
    case RYSPEC_RULE_NUMBER:
      put(o, "{ value = ");
      ryspec_rule_put_number(o, e->number);
      put(o, " }");
      break;
    default:
      break;
    }
  }
  f->sep = true;
  return true;
}

/* ---------------------------------------------------------------------------
 * The parser. */

/* Deeper nesting than this is refused, the parser being recursive. */
#define MAX_DEPTH 256

typedef enum tok {
  T_END,
  T_ERROR, /* a character no token starts with */
  T_LPAREN,
  T_RPAREN,
  T_LBRACKET,
  T_RBRACKET,
  T_LBRACE,
  T_RBRACE,
  T_COMMA,
  T_COLON,
  T_ARROW,  /* -> */
  T_ASSIGN, /* := */
  T_LT,
  T_LE,
  T_GT,
  T_GE,
  T_EQ, /* == */
  T_NE, /* != */
  T_PLUS,
  T_MINUS,
  T_NAME,
  T_NUMBER,
} tok;

typedef struct parser {
  const char *src;
  size_t len, pos;
  int line;
  size_t line_start;
  /* The current token. */
  tok tok;
  size_t start, end;
  int tok_line, tok_column;
  int depth;
  bool in_bound; /* lexing inside a bound */
  bool failed;
  ryspec_rule_sink *sink; /* NULL while checking */
  /* The outermost parenthesis around what comes next, where the node it
   * groups is placed. */
  bool pending;
  int pending_line, pending_column;
  ryspec_diagnostic *err;
} parser;

static bool is_letter(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static void advance(parser *p) {
  while (p->pos < p->len) {
    char c = p->src[p->pos];
    if (c == '\n') {
      p->line++;
      p->line_start = p->pos + 1;
    } else if (c != ' ' && c != '\t' && c != '\r') {
      break;
    }
    p->pos++;
  }
  p->start = p->pos;
  p->tok_line = p->line;
  p->tok_column = (int)(p->pos - p->line_start) + 1;
  if (p->pos == p->len) {
    p->tok = T_END;
    p->end = p->pos;
    return;
  }
  const char *s = p->src + p->pos;
  size_t left = p->len - p->pos;
  size_t n = 1;
  switch (s[0]) {
  case '(': p->tok = T_LPAREN; break;
  case ')': p->tok = T_RPAREN; break;
  case '[': p->tok = T_LBRACKET; break;
  case ']': p->tok = T_RBRACKET; break;
  case '{': p->tok = T_LBRACE; break;
  case '}': p->tok = T_RBRACE; break;
  case ',': p->tok = T_COMMA; break;
  case '+': p->tok = T_PLUS; break;
  case ':':
    p->tok = left > 1 && s[1] == '=' && !p->in_bound ? (n = 2, T_ASSIGN)
                                                     : T_COLON;
    break;
  case '-':
    p->tok = left > 1 && s[1] == '>' ? (n = 2, T_ARROW) : T_MINUS;
    break;
  case '<':
    p->tok = left > 1 && s[1] == '=' ? (n = 2, T_LE) : T_LT;
    break;
  case '>':
    p->tok = left > 1 && s[1] == '=' ? (n = 2, T_GE) : T_GT;
    break;
  case '=':
    p->tok = left > 1 && s[1] == '=' ? (n = 2, T_EQ) : T_ERROR;
    break;
  case '!':
    p->tok = left > 1 && s[1] == '=' ? (n = 2, T_NE) : T_ERROR;
    break;
  default:
    if (is_letter(s[0])) {
      while (n < left && (is_letter(s[n]) || is_digit(s[n]))) {
        n++;
      }
      p->tok = T_NAME;
    } else if (is_digit(s[0])) {
      while (n < left && is_digit(s[n])) {
        n++;
      }
      if (n + 1 < left && s[n] == '.' && is_digit(s[n + 1])) {
        for (n += 2; n < left && is_digit(s[n]); n++) {
        }
      }
      size_t e = n;
      if (e < left && (s[e] == 'e' || s[e] == 'E')) {
        e++;
        if (e < left && (s[e] == '+' || s[e] == '-')) {
          e++;
        }
        if (e < left && is_digit(s[e])) {
          for (n = e; n < left && is_digit(s[n]); n++) {
          }
        }
      }
      p->tok = T_NUMBER;
    } else {
      p->tok = T_ERROR;
    }
  }
  p->pos += n;
  p->end = p->pos;
}

/* Whether the current token is the word w. */
static bool word(const parser *p, const char *w) {
  size_t n = strlen(w);
  return p->tok == T_NAME && p->end - p->start == n &&
         memcmp(p->src + p->start, w, n) == 0;
}

static void fail(parser *p, const char *expected) {
  if (p->failed) {
    return;
  }
  p->failed = true;
  p->err->status = RYSPEC_ERROR_GRAMMAR;
  p->err->line = p->tok_line;
  p->err->column = p->tok_column;
  if (p->tok == T_END) {
    snprintf(p->err->message, sizeof p->err->message,
             "expected %s, found the end of the expression", expected);
  } else {
    int n = (int)(p->end - p->start);
    snprintf(p->err->message, sizeof p->err->message,
             "expected %s, found `%.*s`", expected, n > 40 ? 40 : n,
             p->src + p->start);
  }
}

/* Consume a token t, or fail expecting what. */
static bool expect(parser *p, tok t, const char *what) {
  if (p->failed) {
    return false;
  }
  if (p->tok != t) {
    fail(p, what);
    return false;
  }
  advance(p);
  return true;
}

static void quantified(parser *p);
static void unary(parser *p);

/* Where the node opening at line and column is placed: at a parenthesis
 * waiting for it, if one is. */
static void place(parser *p, int *line, int *column) {
  if (p->pending) {
    *line = p->pending_line;
    *column = p->pending_column;
    p->pending = false;
  }
}

/* The number token, read. */
static double number(const parser *p) {
  char buf[64];
  size_t n = p->end - p->start;
  if (n >= sizeof buf) {
    n = sizeof buf - 1;
  }
  memcpy(buf, p->src + p->start, n);
  buf[n] = '\0';
  return strtod(buf, NULL);
}

/* An end of a bound, set on bound as its max or min. */
static void end(parser *p, ryspec_rule_bound *bound, bool max) {
  if (p->tok == T_NUMBER) {
    ryspec_rule_bound_end(bound, max, number(p), NULL, 0);
  } else {
    ryspec_rule_bound_end(bound, max, 0, p->src + p->start, p->end - p->start);
  }
  advance(p);
}

static bool at_end_token(const parser *p) {
  return p->tok == T_NUMBER || p->tok == T_NAME;
}

/* bound = "[" end ":" [ end ] [ ":" unit ] "]"
 *       | "[" ":" end [ ":" unit ] "]" ; into *out, returning whether one
 * follows. */
static bool bound(parser *p, ryspec_rule_bound *out) {
  if (p->failed || p->tok != T_LBRACKET) {
    return false;
  }
  p->in_bound = true;
  advance(p);
  if (at_end_token(p)) {
    end(p, out, false);
    if (!expect(p, T_COLON, "`:`")) {
      return true;
    }
    if (at_end_token(p)) {
      end(p, out, true);
    }
  } else {
    if (!expect(p, T_COLON, "a bound's end or `:`")) {
      return true;
    }
    if (!at_end_token(p)) {
      fail(p, "a bound's upper end");
      return true;
    }
    end(p, out, true);
  }
  if (p->tok == T_COLON) {
    advance(p);
    const char *unit = p->tok == T_NAME ? ryspec_rule_unit(p->src + p->start,
                                                           p->end - p->start)
                                        : NULL;
    if (!unit) {
      fail(p, "a time unit");
      return true;
    }
    out->time_unit = unit;
    advance(p);
  }
  /* The token after the bracket is lexed outside it. */
  p->in_bound = false;
  expect(p, T_RBRACKET, "`]`");
  return true;
}

/* atom = "{" name "}"
 *      | "{" name comparison ( name | signed-number ) "}"
 *      | "{" name ":=" name "}" ; */
static void atom(parser *p) {
  int line = p->tok_line, column = p->tok_column;
  place(p, &line, &column);
  advance(p);
  if (p->tok != T_NAME) {
    fail(p, "a name");
    return;
  }
  const char *name = p->src + p->start;
  size_t name_len = p->end - p->start;
  int name_line = p->tok_line, name_column = p->tok_column;
  advance(p);
  ryspec_rule_op op;
  switch (p->tok) {
  case T_RBRACE:
    ryspec_rule_leaf(p->sink, RYSPEC_RULE_REFERENCE, name, name_len, line,
                     column);
    break;
  case T_ASSIGN:
    advance(p);
    if (p->tok != T_NAME) {
      fail(p, "a variable to assign from");
      return;
    }
    ryspec_rule_enter(p->sink, RYSPEC_RULE_OP_ASSIGN, NULL, line, column);
    ryspec_rule_leaf(p->sink, RYSPEC_RULE_REFERENCE, p->src + p->start,
                     p->end - p->start, p->tok_line, p->tok_column);
    ryspec_rule_leaf(p->sink, RYSPEC_RULE_QVAR, name, name_len, name_line,
                     name_column);
    ryspec_rule_leave(p->sink, RYSPEC_RULE_OP_ASSIGN, NULL, line, column);
    advance(p);
    break;
  case T_LT: op = RYSPEC_RULE_OP_LT; goto comparison;
  case T_LE: op = RYSPEC_RULE_OP_LE; goto comparison;
  case T_GT: op = RYSPEC_RULE_OP_GT; goto comparison;
  case T_GE: op = RYSPEC_RULE_OP_GE; goto comparison;
  case T_EQ: op = RYSPEC_RULE_OP_EQ; goto comparison;
  case T_NE: op = RYSPEC_RULE_OP_NE;
  comparison:
    advance(p);
    if (p->tok == T_NAME) {
      ryspec_rule_enter(p->sink, op, NULL, line, column);
      ryspec_rule_leaf(p->sink, RYSPEC_RULE_REFERENCE, name, name_len,
                       name_line, name_column);
      ryspec_rule_leaf(p->sink, RYSPEC_RULE_REFERENCE, p->src + p->start,
                       p->end - p->start, p->tok_line, p->tok_column);
      ryspec_rule_leave(p->sink, op, NULL, line, column);
      advance(p);
    } else {
      /* A sign stands against its digits, as one token. */
      int sign_line = p->tok_line, sign_column = p->tok_column;
      double sign = 1;
      if ((p->tok == T_PLUS || p->tok == T_MINUS) && p->end < p->len &&
          is_digit(p->src[p->end])) {
        sign = p->tok == T_MINUS ? -1 : 1;
        advance(p);
      }
      if (p->tok != T_NUMBER) {
        fail(p, "a name or a number");
        return;
      }
      ryspec_rule_enter(p->sink, op, NULL, line, column);
      ryspec_rule_leaf(p->sink, RYSPEC_RULE_REFERENCE, name, name_len,
                       name_line, name_column);
      ryspec_rule_number(p->sink, sign * number(p), sign_line, sign_column);
      ryspec_rule_leave(p->sink, op, NULL, line, column);
      advance(p);
    }
    break;
  default:
    fail(p, "`}`, `:=` or a comparison");
    return;
  }
  expect(p, T_RBRACE, "`}`");
}

/* primary = atom | "(" quantified ")" ; */
static void primary(parser *p) {
  if (p->tok == T_LBRACE) {
    atom(p);
    return;
  }
  if (p->tok == T_LPAREN) {
    if (!p->pending) {
      p->pending = true;
      p->pending_line = p->tok_line;
      p->pending_column = p->tok_column;
    }
    advance(p);
    quantified(p);
    expect(p, T_RPAREN, "`)`");
    return;
  }
  fail(p, "an operand");
}

static bool enter(parser *p) {
  if (++p->depth > MAX_DEPTH) {
    if (!p->failed) {
      fail(p, "less nesting");
    }
    return false;
  }
  return true;
}

/* unary = ( "not" | "next" ) unary
 *       | unary-temporal [ bound ] unary
 *       | primary ; */
static void unary(parser *p) {
  if (p->failed || !enter(p)) {
    return;
  }
  static const struct {
    const char *word;
    ryspec_rule_op op;
    bool bounded;
  } ops[] = {
      {"not", RYSPEC_RULE_OP_NOT, false},
      {"next", RYSPEC_RULE_OP_NEXT, false},
      {"once", RYSPEC_RULE_OP_ONCE, true},
      {"historically", RYSPEC_RULE_OP_HISTORICALLY, true},
      {"eventually", RYSPEC_RULE_OP_EVENTUALLY, true},
      {"always", RYSPEC_RULE_OP_ALWAYS, true},
  };
  for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++) {
    if (word(p, ops[i].word)) {
      int line = p->tok_line, column = p->tok_column;
      place(p, &line, &column);
      advance(p);
      ryspec_rule_bound b = {0};
      bool has = ops[i].bounded && bound(p, &b);
      ryspec_rule_enter(p->sink, ops[i].op, has ? &b : NULL, line, column);
      unary(p);
      ryspec_rule_leave(p->sink, ops[i].op, has ? &b : NULL, line, column);
      p->depth--;
      return;
    }
  }
  primary(p);
  p->depth--;
}

/* The levels of the binary operators, loosest first. */
typedef enum level { L_IMPLIES, L_OR, L_AND, L_TEMPORAL } level;

/* Whether the current token is a binary operator: which, in *op, and of
 * which level, in *lv. Outside braces and brackets, a word is a keyword. */
static bool binary(const parser *p, level *lv, ryspec_rule_op *op) {
  static const struct {
    const char *word;
    level lv;
    ryspec_rule_op op;
  } words[] = {
      {"implies", L_IMPLIES, RYSPEC_RULE_OP_IMPLIES},
      {"or", L_OR, RYSPEC_RULE_OP_OR},
      {"xor", L_OR, RYSPEC_RULE_OP_XOR},
      {"and", L_AND, RYSPEC_RULE_OP_AND},
      {"since", L_TEMPORAL, RYSPEC_RULE_OP_SINCE},
      {"until", L_TEMPORAL, RYSPEC_RULE_OP_UNTIL},
  };
  if (p->tok == T_ARROW) {
    *lv = L_IMPLIES;
    *op = RYSPEC_RULE_OP_IMPLIES;
    return true;
  }
  for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) {
    if (word(p, words[i].word)) {
      *lv = words[i].lv;
      *op = words[i].op;
      return true;
    }
  }
  return false;
}

/* The runs of operators of level lv ahead of p, which the level's span
 * holds outside every bracket: from p up to a looser operator, a closer it
 * did not open, or the end. A run is a chain of one operator, gathered into
 * one node; at L_TEMPORAL, each operator is a run of its own. Returns how
 * many runs there are; when i is not 0, stops at the first operator of the
 * i-th, with *at the parser there. */
static size_t runs(const parser *p, level lv, size_t i, parser *at) {
  parser q = *p;
  size_t depth = 0, count = 0;
  bool any = false;
  ryspec_rule_op last = RYSPEC_RULE_OP_NOT;
  for (; q.tok != T_END; advance(&q)) {
    switch (q.tok) {
    case T_LPAREN:
    case T_LBRACKET:
    case T_LBRACE:
      depth++;
      continue;
    case T_RPAREN:
    case T_RBRACKET:
    case T_RBRACE:
      if (depth == 0) {
        return count;
      }
      depth--;
      continue;
    default:
      break;
    }
    level l;
    ryspec_rule_op op;
    if (depth > 0 || !binary(&q, &l, &op) || l > lv) {
      continue;
    }
    if (l < lv) {
      return count;
    }
    if (!any || op != last || lv == L_TEMPORAL) {
      if (++count == i) {
        *at = q;
        return count;
      }
    }
    any = true;
    last = op;
  }
  return count;
}

/* A level whose operators gather on the left, or at L_TEMPORAL bind to the
 * left, two operands each:
 *   temporal    = unary { ( "since" | "until" ) [ bound ] unary } ;
 *   conjunction = temporal { "and" temporal } ;
 *   disjunction = conjunction { ( "or" | "xor" ) conjunction } ;
 * Each node sits where its first operand starts, but the outermost, which
 * takes a parenthesis waiting for it. */
static void left(parser *p, level lv) {
  int line = p->tok_line, column = p->tok_column;
  int outer_line = line, outer_column = column;
  size_t count = p->sink ? runs(p, lv, 0, NULL) : 0;
  for (size_t i = count; i > 0; i--) {
    parser at;
    runs(p, lv, i, &at);
    level l;
    ryspec_rule_op op;
    binary(&at, &l, &op);
    advance(&at);
    ryspec_rule_bound b = {0};
    bool has = lv == L_TEMPORAL && bound(&at, &b);
    if (i == count) {
      place(p, &outer_line, &outer_column);
    }
    ryspec_rule_enter(p->sink, op, has ? &b : NULL,
                      i == count ? outer_line : line,
                      i == count ? outer_column : column);
  }

  if (lv == L_TEMPORAL) {
    unary(p);
  } else {
    left(p, lv + 1);
  }
  size_t closed = 0;
  bool open = false;
  ryspec_rule_op run = RYSPEC_RULE_OP_NOT;
  ryspec_rule_bound b = {0};
  bool has = false;
  level l;
  ryspec_rule_op op;
  while (!p->failed && binary(p, &l, &op) && l == lv) {
    if (open && (op != run || lv == L_TEMPORAL)) {
      closed++;
      ryspec_rule_leave(p->sink, run, has ? &b : NULL,
                        closed == count ? outer_line : line,
                        closed == count ? outer_column : column);
    }
    open = true;
    run = op;
    advance(p);
    b = (ryspec_rule_bound){0};
    has = lv == L_TEMPORAL && bound(p, &b);
    if (lv == L_TEMPORAL) {
      unary(p);
    } else {
      left(p, lv + 1);
    }
  }
  if (open) {
    closed++;
    ryspec_rule_leave(p->sink, run, has ? &b : NULL,
                      closed == count ? outer_line : line,
                      closed == count ? outer_column : column);
  }
}

/* implication = disjunction [ ( "->" | "implies" ) implication ] ; one
 * node, from the first operand of a chain to its last, unless in_chain,
 * when this is a later operand of one already open. */
static void implication(parser *p, bool in_chain) {
  if (!enter(p)) {
    return;
  }
  int line = p->tok_line, column = p->tok_column;
  bool opens = p->sink && !in_chain && runs(p, L_IMPLIES, 0, NULL) > 0;
  if (opens) {
    place(p, &line, &column);
    ryspec_rule_enter(p->sink, RYSPEC_RULE_OP_IMPLIES, NULL, line, column);
  }
  left(p, L_OR);
  if (!p->failed && (p->tok == T_ARROW || word(p, "implies"))) {
    advance(p);
    implication(p, true);
  }
  if (opens) {
    ryspec_rule_leave(p->sink, RYSPEC_RULE_OP_IMPLIES, NULL, line, column);
  }
  p->depth--;
}

/* binder = "[" name { "," name } "]" ; each name a BIND when emit. */
static void binder(parser *p, bool emit) {
  if (!expect(p, T_LBRACKET, "`[` and the names it binds")) {
    return;
  }
  for (;;) {
    if (p->tok != T_NAME) {
      fail(p, "a name to bind");
      return;
    }
    if (emit) {
      ryspec_rule_leaf(p->sink, RYSPEC_RULE_BIND, p->src + p->start,
                       p->end - p->start, p->tok_line, p->tok_column);
    }
    advance(p);
    if (p->tok != T_COMMA) {
      break;
    }
    advance(p);
  }
  expect(p, T_RBRACKET, "`,` or `]`");
}

/* quantified = quantifier binder quantified | implication ; the names
 * bound come after the operand, as in prefix form, read again from the
 * binder. */
static void quantified(parser *p) {
  if (p->failed || !enter(p)) {
    return;
  }
  if (word(p, "forall") || word(p, "exists")) {
    ryspec_rule_op op =
        word(p, "forall") ? RYSPEC_RULE_OP_FORALL : RYSPEC_RULE_OP_EXISTS;
    int line = p->tok_line, column = p->tok_column;
    place(p, &line, &column);
    ryspec_rule_enter(p->sink, op, NULL, line, column);
    advance(p);
    parser names = *p;
    binder(p, false);
    quantified(p);
    if (p->sink) {
      binder(&names, true);
    }
    ryspec_rule_leave(p->sink, op, NULL, line, column);
  } else {
    implication(p, false);
  }
  p->depth--;
}

/* expression = "(" quantified ")" ; into sink, which may be NULL. */
static bool parse(const char *src, size_t len, ryspec_rule_sink *sink,
                  ryspec_diagnostic *err) {
  parser p = {.src = src, .len = len, .line = 1, .err = err, .sink = sink};
  advance(&p);
  if (expect(&p, T_LPAREN, "`(`")) {
    quantified(&p);
    /* The closing `)`, if it is one: the expression's last byte, since
     * whitespace separates two tokens and none follows the last. */
    size_t close = p.end;
    int close_line = p.tok_line, close_column = p.tok_column;
    if (expect(&p, T_RPAREN, "`)`")) {
      if (p.tok != T_END) {
        fail(&p, "the end of the expression");
      } else if (close != len) {
        p.failed = true;
        *err = (ryspec_diagnostic){RYSPEC_ERROR_GRAMMAR, close_line,
                                   close_column + 1, ""};
        snprintf(err->message, sizeof err->message,
                 "whitespace after the closing `)`, where the expression "
                 "ends");
      }
    }
  }
  return !p.failed;
}

bool ryspec_expr_walk(const char *src, size_t len, ryspec_rule_sink *sink,
                      ryspec_diagnostic *diag) {
  ryspec_diagnostic ignored;
  if (!diag) {
    diag = &ignored;
  }
  *diag = (ryspec_diagnostic){.status = RYSPEC_OK};
  return parse(src, len, NULL, diag) &&
         (!sink || parse(src, len, sink, diag));
}
