/* Expressions into prefix form: each expression-form string at a rule
 * position is walked (expr_parser.h) into prefix-form TOML text, one line
 * `r<i> = [...]` per expression; tomlc17 parses the lines, and the array of
 * each replaces its string in the document's tree, placed where the
 * expression spells each part of it. The twins' strings are copied into
 * the document's block, after the document, and their result freed: the
 * document holds the prefix form alone, nothing in its tree pointing at an
 * expression.
 *
 * This writes into tomlc17's tree, as of the tag CMake pins: a value is a
 * toml_datum_t held by value in its parent's array, a table's or an
 * array's own arrays belong to the tree, freed with it by toml_free(), and
 * its strings to the pool of the result that parsed them. So an array moved
 * from one result's tree into another's is freed by the second, its slot in
 * the first zeroed so it is not freed twice; its strings, which the first
 * result's pool holds, are copied out before that result is freed. */
#include <stdio.h>
#include <string.h>

#include "expr_parser.h"
#include "toml_doc.h"

/* ---------------------------------------------------------------------------
 * Rule positions. */

/* Receives each expression-form string; false stops the walk. */
typedef bool (*expression_fn)(toml_datum_t* v, void* ctx);

static toml_datum_t* get(toml_datum_t* t, const char* key)
{
  return ryspec_toml_value_lookup(t, key, strlen(key));
}

static bool at(toml_datum_t* v, expression_fn fn, void* ctx)
{
  if(
    !v || v->type != TOML_STRING || v->u.str.len == 0 ||
    v->u.str.ptr[0] != '(') {
    return true;
  }
  return fn(v, ctx);
}

/* Every value of table t, each a rule position. */
static bool each_value(toml_datum_t* t, expression_fn fn, void* ctx)
{
  if(!t || t->type != TOML_TABLE) {
    return true;
  }
  for(int i = 0; i < t->u.tab.size; i++) {
    if(!at(&t->u.tab.value[i], fn, ctx)) {
      return false;
    }
  }
  return true;
}

/* The rule positions of the namespace ns: its rules, and each property's
 * `given`, `check` and private rules. */
static bool in_namespace(toml_datum_t* ns, expression_fn fn, void* ctx)
{
  if(!each_value(get(ns, "rules"), fn, ctx)) {
    return false;
  }
  toml_datum_t* properties = get(ns, "properties");
  if(!properties || properties->type != TOML_TABLE) {
    return true;
  }
  for(int i = 0; i < properties->u.tab.size; i++) {
    toml_datum_t* p = &properties->u.tab.value[i];
    if(
      !at(get(p, "given"), fn, ctx) || !at(get(p, "check"), fn, ctx) ||
      !each_value(get(p, "where"), fn, ctx)) {
      return false;
    }
  }
  return true;
}

/* The rule positions of the named namespaces under v, a table of them. */
static bool under(toml_datum_t* v, expression_fn fn, void* ctx)
{
  if(!v || v->type != TOML_TABLE) {
    return true;
  }
  for(int i = 0; i < v->u.tab.size; i++) {
    const char* key = v->u.tab.key[i];
    toml_datum_t* ns = &v->u.tab.value[i];
    if(
      ns->type != TOML_TABLE || strcmp(key, "rules") == 0 ||
      strcmp(key, "properties") == 0 || strcmp(key, "extras") == 0) {
      continue;
    }
    if(!in_namespace(ns, fn, ctx) || !under(ns, fn, ctx)) {
      return false;
    }
  }
  return true;
}

static bool each_expression(toml_datum_t* root, expression_fn fn, void* ctx)
{
  return in_namespace(root, fn, ctx) && under(get(root, "namespace"), fn, ctx);
}

/* ---------------------------------------------------------------------------
 * Text and places. */

/* Where an event is spelled. */
typedef struct place {
  int line, column;
} place;

/* What the walks over the expressions share: the text written, the place
 * of each event, and how many of each so far; with no buffer, a count. */
typedef struct translation {
  ryspec_rule_formatter f;
  place* places; /* NULL to count */
  size_t events, expressions;
  const toml_datum_t* in; /* the string the expression is in */
  ryspec_diag* diag;
} translation;

/* Where in the document the line and column of an expression in the string
 * v lie. Exact but where an escape, or a character of more than one byte,
 * comes before it on its line: the column counts the bytes the string
 * holds, not those that spell it. */
static place placed(const toml_datum_t* v, int line, int column)
{
  if(line > 1) {
    return (place){v->lineno + line - 1, column};
  }
  return (place){v->lineno, v->colno + column - 1};
}

static bool event(const ryspec_rule_event* e, void* ctx)
{
  translation* t = ctx;
  if(t->places) {
    t->places[t->events] = placed(t->in, e->line, e->column);
  }
  t->events++;
  return ryspec_rule_format_event(e, &t->f);
}

/* Write `r<i> = ` and the prefix twin of src, then a newline; when src
 * does not parse, diag is filled, placed in src. */
static bool translate_one(
  translation* t, const char* src, size_t len, ryspec_diag* diag)
{
  char key[32];
  snprintf(key, sizeof key, "r%zu = ", t->expressions++);
  ryspec_rule_put_bytes(&t->f.out, key, strlen(key));
  t->f.sep = false;
  ryspec_rule_sink sink = {.fn = event, .ctx = t};
  if(!ryspec_expr_walk(src, len, &sink, diag)) {
    return false;
  }
  ryspec_rule_put_bytes(&t->f.out, "\n", 1);
  return true;
}

static bool write_expression(toml_datum_t* v, void* ctx)
{
  translation* t = ctx;
  t->in = v;
  if(!translate_one(t, v->u.str.ptr, (size_t)v->u.str.len, t->diag)) {
    if(t->diag) {
      /* From the fault's place in the string to its place in the document. */
      place at = placed(v, t->diag->line, t->diag->column);
      t->diag->line = at.line;
      t->diag->column = at.column;
    }
    return false;
  }
  return true;
}

/* ---------------------------------------------------------------------------
 * The twins, placed and moved. */

static void set(toml_datum_t* d, place p)
{
  d->lineno = p.line;
  d->colno = p.column;
}

/* Place the twin d, and every value in it, taking the places of its events
 * from *next in the order they came: an operator at its ENTER, its name
 * and its keyword table with it, a bound name at its BIND; each LEAVE's
 * place is passed over. */
static void place_twin(toml_datum_t* d, const place** next)
{
  if(d->type == TOML_STRING) {
    set(d, *(*next)++);
    return;
  }
  if(d->type == TOML_TABLE) {
    /* { value = ... } or { qvar = ... }: the literal's or variable's. */
    place p = *(*next)++;
    set(d, p);
    for(int i = 0; i < d->u.tab.size; i++) {
      set(&d->u.tab.value[i], p);
    }
    return;
  }
  if(d->type != TOML_ARRAY || d->u.arr.size == 0) {
    return;
  }
  place p = *(*next)++;
  toml_datum_t* elem = d->u.arr.elem;
  set(d, p);
  set(&elem[0], p);
  ryspec_rule_op op = RYSPEC_RULE_OP_NOT;
  ryspec_rule_op_from_name(elem[0].u.s, &op);
  /* A comparison's or assign's table is an operand; any other operator's
   * is its keywords: a bound, or the names a quantifier binds. */
  bool table_is_operand =
    (op >= RYSPEC_RULE_OP_LT && op <= RYSPEC_RULE_OP_ENDSWITH) ||
    op == RYSPEC_RULE_OP_ASSIGN;
  for(int i = 1; i < d->u.arr.size; i++) {
    toml_datum_t* e = &elem[i];
    if(e->type != TOML_TABLE || table_is_operand) {
      place_twin(e, next);
      continue;
    }
    set(e, p);
    for(int j = 0; j < e->u.tab.size; j++) {
      toml_datum_t* kw = &e->u.tab.value[j];
      set(kw, p);
      for(int k = 0; kw->type == TOML_ARRAY && k < kw->u.arr.size; k++) {
        set(&kw->u.arr.elem[k], *(*next)++);
      }
    }
  }
  (*next)++;
}

/* What the walk that moves the twins carries. */
typedef struct moving {
  toml_datum_t* twins; /* the values of the twins' table, in order */
  const place* next;
} moving;

static bool move_twin(toml_datum_t* v, void* ctx)
{
  moving* m = ctx;
  toml_datum_t* twin = m->twins++;
  place_twin(twin, &m->next);
  *v = *twin;
  *twin = (toml_datum_t){0};
  return true;
}

/* The expression of the n-th line of the twins, 1-based. */
typedef struct nth {
  int left;
  toml_datum_t* found;
} nth;

static bool find_nth(toml_datum_t* v, void* ctx)
{
  nth* n = ctx;
  if(--n->left == 0) {
    n->found = v;
    return false;
  }
  return true;
}

/* ---------------------------------------------------------------------------
 * The translation. */

/* Parse the twins written into t's buffer into *out, failing with the TOML
 * status of the twin refused, placed at the string of the n-th expression
 * of root, the twin of the n-th line. */
static int parse_twins(
  const translation* t,
  toml_result_t* out,
  toml_datum_t* root,
  ryspec_diag* diag)
{
  *out = toml_parse(t->f.out.buf, (int)t->f.out.len);
  if(out->ok) {
    return RYSPEC_OK;
  }
  ryspec_diag ignored;
  if(!diag) {
    diag = &ignored;
  }
  ryspec_toml_error(out->errmsg, diag);
  char why[sizeof diag->message];
  snprintf(why, sizeof why, "%s", diag->message);
  nth n = {.left = diag->line};
  if(n.left > 0) {
    each_expression(root, find_nth, &n);
  }
  diag->line = n.found ? n.found->lineno : 1;
  diag->column = n.found ? n.found->colno : 1;
  snprintf(
    diag->message,
    sizeof diag->message,
    "the expression's prefix form is refused: %.200s",
    why);
  toml_free(*out);
  *out = (toml_result_t){0};
  return diag->status;
}

static int out_of_memory(ryspec_diag* diag)
{
  if(diag) {
    *diag = (ryspec_diag){.status = RYSPEC_ERROR_MEMORY};
    snprintf(diag->message, sizeof diag->message, "out of memory");
  }
  return RYSPEC_ERROR_MEMORY;
}

/* Allocate t's places and text, as counted, through opt; false when out of
 * memory, diag filled. */
static bool allocate(translation* t, toml_option_t opt, ryspec_diag* diag)
{
  size_t places = t->events * sizeof(place);
  size_t text = t->f.out.len + 1;
  char* block = opt.mem_realloc(NULL, places + text);
  if(!block) {
    out_of_memory(diag);
    return false;
  }
  *t = (translation){
    .places = (place*)(void*)block,
    .f = {.out = {.buf = block + places, .size = text}},
    .diag = t->diag,
  };
  return true;
}

/* Copy the n bytes at *s, and a NUL, to *to, advancing it; *s points at
 * the copy. With no *to, nothing is copied. Returns the bytes. */
static size_t own(const char** s, size_t n, char** to)
{
  if(*to) {
    memcpy(*to, *s, n);
    (*to)[n] = '\0';
    *s = *to;
    *to += n + 1;
  }
  return n + 1;
}

/* The bytes of the strings and keys in d; with *to, each is copied there
 * and d pointed at its copy. */
static size_t own_strings(toml_datum_t* d, char** to)
{
  size_t n = 0;
  if(d->type == TOML_STRING) {
    n += own(&d->u.str.ptr, (size_t)d->u.str.len, to);
  }
  else if(d->type == TOML_ARRAY) {
    for(int i = 0; i < d->u.arr.size; i++) {
      n += own_strings(&d->u.arr.elem[i], to);
    }
  }
  else if(d->type == TOML_TABLE) {
    for(int i = 0; i < d->u.tab.size; i++) {
      n += own(&d->u.tab.key[i], (size_t)d->u.tab.len[i], to);
      n += own_strings(&d->u.tab.value[i], to);
    }
  }
  return n;
}

/* Grow *doc's block to hold the strings of every twin in twins after the
 * document, and copy them there, so their result can be freed once the
 * twins are moved; false, *doc unchanged and diag filled, when out of
 * memory. */
static bool own_twins(
  ryspec_toml_doc** doc,
  toml_result_t* twins,
  toml_option_t opt,
  ryspec_diag* diag)
{
  toml_datum_t* top = &twins->toptab;
  char* to = NULL;
  size_t n = 0;
  for(int i = 0; i < top->u.tab.size; i++) {
    n += own_strings(&top->u.tab.value[i], &to);
  }
  ryspec_toml_doc* grown = opt.mem_realloc(*doc, sizeof **doc + n);
  if(!grown) {
    out_of_memory(diag);
    return false;
  }
  *doc = grown;
  to = (char*)(grown + 1);
  for(int i = 0; i < top->u.tab.size; i++) {
    own_strings(&top->u.tab.value[i], &to);
  }
  return true;
}

int ryspec_translate(
  ryspec_toml_doc** doc, toml_option_t opt, ryspec_diag* diag)
{
  toml_datum_t* root = &(*doc)->result.toptab;
  translation t = {.diag = diag};
  if(!each_expression(root, write_expression, &t)) {
    return diag ? diag->status : RYSPEC_ERROR_GRAMMAR;
  }
  if(t.expressions == 0) {
    return RYSPEC_OK;
  }
  if(!allocate(&t, opt, diag)) {
    return RYSPEC_ERROR_MEMORY;
  }
  each_expression(root, write_expression, &t);
  ryspec_rule_out_finish(&t.f.out);

  toml_result_t twins;
  int status = parse_twins(&t, &twins, root, diag);
  if(status == RYSPEC_OK) {
    if(own_twins(doc, &twins, opt, diag)) {
      moving m = {.twins = twins.toptab.u.tab.value, .next = t.places};
      each_expression(&(*doc)->result.toptab, move_twin, &m);
    }
    else {
      status = RYSPEC_ERROR_MEMORY;
    }
    toml_free(twins);
  }
  opt.mem_free(t.places);
  return status;
}
