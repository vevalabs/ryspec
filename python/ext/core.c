/* ryspec._core: the Python binding of libryspec.
 *
 * lint(path, rules=None) parses the file at path and lints it, against
 * every rule libryspec checks or, given rules, against those alone, in
 * order, 0 being the schema. It returns its findings as a list of (kind,
 * line, column, message) tuples: empty when there are none, else the first,
 * which is the reason the file failed to parse or its first violation.
 * lint_rules() gives what libryspec checks, and library_version the version
 * of the libryspec it was built from.
 *
 * index(path) parses the file at path and returns its entities, by id, as a
 * list of dicts: id, kind, name, path, parent, line and column, each but id
 * and kind None where the entity has none. A file that does not parse
 * raises OSError, MemoryError, or ValueError(kind, line, column, message). */
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "ryspec/ryspec.h"

static const char *kind_name(ryspec_status status) {
  switch (status) {
  case RYSPEC_ERROR_IO:
    return "io";
  case RYSPEC_ERROR_MEMORY:
    return "memory";
  case RYSPEC_ERROR_TOML:
  case RYSPEC_ERROR_TOML_ENCODING:
  case RYSPEC_ERROR_TOML_REDEFINED:
  case RYSPEC_ERROR_TOML_LIMIT:
    return "toml";
  case RYSPEC_ERROR_SCHEMA:
    return "schema";
  case RYSPEC_ERROR_GRAMMAR:
    return "grammar";
  case RYSPEC_ERROR_SEMANTIC:
    return "semantic";
  default:
    return "unknown";
  }
}

static bool is_checked(long rule) {
  size_t n;
  const ryspec_lint_rule *rules = ryspec_lint_rules(&n);
  for (size_t i = 0; i < n; i++) {
    if ((long)rules[i] == rule) {
      return true;
    }
  }
  return false;
}

/* The rules of the iterable obj, each one libryspec checks, into a new
 * array of *count, which the caller frees with PyMem_Free; NULL with the
 * Python error set on failure. */
static ryspec_lint_rule *rules_from(PyObject *obj, size_t *count) {
  PyObject *seq = PySequence_Fast(obj, "rules must be an iterable of ints");
  if (!seq) {
    return NULL;
  }
  Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
  ryspec_lint_rule *rules = PyMem_New(ryspec_lint_rule, n ? n : 1);
  if (!rules) {
    Py_DECREF(seq);
    PyErr_NoMemory();
    return NULL;
  }
  for (Py_ssize_t i = 0; i < n; i++) {
    long rule = PyLong_AsLong(PySequence_Fast_GET_ITEM(seq, i));
    if (rule == -1 && PyErr_Occurred()) {
      goto fail;
    }
    if (!is_checked(rule)) {
      PyErr_Format(PyExc_ValueError, "no lint rule %ld", rule);
      goto fail;
    }
    rules[i] = (ryspec_lint_rule)rule;
  }
  Py_DECREF(seq);
  *count = (size_t)n;
  return rules;

fail:
  Py_DECREF(seq);
  PyMem_Free(rules);
  return NULL;
}

static PyObject *lint(PyObject *self, PyObject *args, PyObject *kwargs) {
  (void)self;
  static char *keywords[] = {"path", "rules", NULL};
  PyObject *path_obj, *rules_obj = Py_None;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O&|O", keywords,
                                   PyUnicode_FSConverter, &path_obj,
                                   &rules_obj)) {
    return NULL;
  }
  ryspec_lint_rule *rules = NULL;
  size_t n_rules = 0;
  if (rules_obj != Py_None && !(rules = rules_from(rules_obj, &n_rules))) {
    Py_DECREF(path_obj);
    return NULL;
  }

  ryspec_diagnostic diag;
  ryspec_status status;
  Py_BEGIN_ALLOW_THREADS
  ryspec_toml_doc *doc =
      ryspec_toml_parse_file(PyBytes_AS_STRING(path_obj), &diag);
  status = diag.status;
  if (doc && !rules) {
    status = ryspec_toml_lint(doc, &diag);
  }
  for (size_t i = 0; doc && rules && i < n_rules && status == RYSPEC_OK;
       i++) {
    status = ryspec_toml_lint_rule(doc, rules[i], &diag);
  }
  ryspec_toml_doc_free(doc);
  Py_END_ALLOW_THREADS
  Py_DECREF(path_obj);
  PyMem_Free(rules);

  if (status == RYSPEC_OK) {
    return PyList_New(0);
  }
  return Py_BuildValue("[(siis)]", kind_name(status), diag.line, diag.column,
                       diag.message);
}

static const char *entity_kind_name(ryspec_entity_kind kind) {
  switch (kind) {
  case RYSPEC_ENTITY_NAMESPACE:
    return "namespace";
  case RYSPEC_ENTITY_RULE:
    return "rule";
  case RYSPEC_ENTITY_PROPERTY:
    return "property";
  case RYSPEC_ENTITY_PRIVATE_RULE:
    return "private_rule";
  case RYSPEC_ENTITY_VARIABLE:
    return "variable";
  case RYSPEC_ENTITY_MONITOR:
    return "monitor";
  case RYSPEC_ENTITY_GIVEN:
    return "given";
  case RYSPEC_ENTITY_CHECK:
    return "check";
  default:
    return "none";
  }
}

/* Raise the failure diag describes. */
static PyObject *raise(const ryspec_diagnostic *diag) {
  switch (diag->status) {
  case RYSPEC_ERROR_IO:
    return PyErr_Format(PyExc_OSError, "%s", diag->message);
  case RYSPEC_ERROR_MEMORY:
    return PyErr_NoMemory();
  default: {
    PyObject *args = Py_BuildValue("(siis)", kind_name(diag->status),
                                   diag->line, diag->column, diag->message);
    if (args) {
      PyErr_SetObject(PyExc_ValueError, args);
      Py_DECREF(args);
    }
    return NULL;
  }
  }
}

/* The len bytes at s as a str, or None for s NULL. */
static PyObject *text(const char *s, size_t len) {
  if (!s) {
    Py_RETURN_NONE;
  }
  return PyUnicode_DecodeUTF8(s, (Py_ssize_t)len, "surrogateescape");
}

/* The entity e of doc, as a dict. */
static PyObject *entity_dict(const ryspec_toml_doc *doc, ryspec_entity e) {
  size_t name_len;
  const char *name = ryspec_entity_name(doc, e, &name_len);
  size_t path_len = ryspec_entity_path(doc, e, NULL, 0);
  char *path = PyMem_Malloc(path_len + 1);
  if (!path) {
    return PyErr_NoMemory();
  }
  ryspec_entity_path(doc, e, path, path_len + 1);
  ryspec_entity parent = ryspec_entity_parent(doc, e);
  int line, column;
  bool placed = ryspec_entity_place(doc, e, &line, &column);

  PyObject *values[] = {
      PyLong_FromSize_t(e),
      PyUnicode_FromString(entity_kind_name(ryspec_entity_kind_of(doc, e))),
      text(name, name_len),
      text(name ? path : NULL, path_len),
      parent == RYSPEC_NO_ENTITY ? Py_NewRef(Py_None)
                                 : PyLong_FromSize_t(parent),
      placed ? PyLong_FromLong(line) : Py_NewRef(Py_None),
      placed ? PyLong_FromLong(column) : Py_NewRef(Py_None),
  };
  static const char *keys[] = {"id",     "kind", "name",  "path",
                               "parent", "line", "column"};
  PyMem_Free(path);
  PyObject *dict = PyDict_New();
  for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
    if (dict &&
        (!values[i] || PyDict_SetItemString(dict, keys[i], values[i]) < 0)) {
      Py_CLEAR(dict);
    }
    Py_XDECREF(values[i]);
  }
  return dict;
}

static PyObject *index_(PyObject *self, PyObject *args, PyObject *kwargs) {
  (void)self;
  static char *keywords[] = {"path", NULL};
  PyObject *path_obj;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O&", keywords,
                                   PyUnicode_FSConverter, &path_obj)) {
    return NULL;
  }
  ryspec_diagnostic diag;
  ryspec_toml_doc *doc;
  Py_BEGIN_ALLOW_THREADS
  doc = ryspec_toml_parse_file(PyBytes_AS_STRING(path_obj), &diag);
  Py_END_ALLOW_THREADS
  Py_DECREF(path_obj);
  if (!doc) {
    return raise(&diag);
  }

  size_t n = ryspec_entity_count(doc);
  PyObject *list = PyList_New((Py_ssize_t)n);
  for (size_t e = 0; list && e < n; e++) {
    PyObject *dict = entity_dict(doc, e);
    if (!dict) {
      Py_CLEAR(list);
      break;
    }
    PyList_SET_ITEM(list, (Py_ssize_t)e, dict);
  }
  ryspec_toml_doc_free(doc);
  return list;
}

static PyObject *lint_rules(PyObject *self, PyObject *unused) {
  (void)self;
  (void)unused;
  size_t n;
  const ryspec_lint_rule *rules = ryspec_lint_rules(&n);
  PyObject *tuple = PyTuple_New((Py_ssize_t)n);
  for (size_t i = 0; tuple && i < n; i++) {
    PyObject *rule = PyLong_FromLong((long)rules[i]);
    if (!rule) {
      Py_CLEAR(tuple);
      break;
    }
    PyTuple_SET_ITEM(tuple, (Py_ssize_t)i, rule);
  }
  return tuple;
}

static PyMethodDef methods[] = {
    {"lint", (PyCFunction)(void (*)(void))lint, METH_VARARGS | METH_KEYWORDS,
     "lint(path, rules=None) -> list of (kind, line, column, message)\n\n"
     "Parse and lint the ryspec document at path, against every rule\n"
     "libryspec checks, or against rules alone. The list is empty, or holds\n"
     "the first finding."},
    {"index", (PyCFunction)(void (*)(void))index_,
     METH_VARARGS | METH_KEYWORDS,
     "index(path) -> list of dict\n\n"
     "Parse the ryspec document at path and list its entities by id: each a\n"
     "dict of id, kind, name, path, parent, line and column, None where it\n"
     "has none. The kinds are namespace, rule, property, private_rule,\n"
     "variable, monitor, given and check; entity 0 is the root namespace."},
    {"lint_rules", lint_rules, METH_NOARGS,
     "lint_rules() -> tuple of int\n\n"
     "What libryspec checks, in order: 0, the schema, then the rules of\n"
     "SPEC.md by number."},
    {NULL, NULL, 0, NULL},
};

static struct PyModuleDef module = {
    PyModuleDef_HEAD_INIT, "ryspec._core", "The libryspec binding.", -1,
    methods,
};

PyMODINIT_FUNC PyInit__core(void) {
  PyObject *m = PyModule_Create(&module);
  if (m && PyModule_AddStringConstant(m, "library_version",
                                      ryspec_version()) < 0) {
    Py_CLEAR(m);
  }
  return m;
}
