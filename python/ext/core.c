/* ryspec._core: the Python binding of libryspec.
 *
 * lint(path) parses the file at path and lints it, and returns its findings
 * as a list of (kind, line, column, message) tuples, empty when there are
 * none. A file that fails to parse yields the one finding saying why. */
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
    return "toml";
  case RYSPEC_ERROR_SCHEMA:
    return "schema";
  case RYSPEC_ERROR_SEMANTIC:
    return "semantic";
  default:
    return "unknown";
  }
}

/* Appends diag to the list *ctx, or drops the list and sets it to NULL on
 * failure, leaving the Python error set. */
static void append_finding(const ryspec_diagnostic *diag, void *ctx) {
  PyObject **findings = ctx;
  if (!*findings) {
    return;
  }
  PyObject *item = Py_BuildValue("(siis)", kind_name(diag->status),
                                 diag->line, diag->column, diag->message);
  if (!item || PyList_Append(*findings, item) < 0) {
    Py_CLEAR(*findings);
  }
  Py_XDECREF(item);
}

static PyObject *lint(PyObject *self, PyObject *args) {
  (void)self;
  PyObject *path_obj;
  if (!PyArg_ParseTuple(args, "O&", PyUnicode_FSConverter, &path_obj)) {
    return NULL;
  }

  PyObject *findings = PyList_New(0);
  if (!findings) {
    Py_DECREF(path_obj);
    return NULL;
  }

  ryspec_diagnostic diag;
  ryspec_document *doc;
  Py_BEGIN_ALLOW_THREADS
  doc = ryspec_parse_file(PyBytes_AS_STRING(path_obj), &diag);
  Py_END_ALLOW_THREADS
  Py_DECREF(path_obj);

  if (doc) {
    ryspec_lint(doc, append_finding, &findings);
    ryspec_document_free(doc);
  } else {
    append_finding(&diag, &findings);
  }
  return findings;
}

static PyMethodDef methods[] = {
    {"lint", lint, METH_VARARGS,
     "lint(path) -> list of (kind, line, column, message)\n\n"
     "Parse and lint the ryspec document at path."},
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
