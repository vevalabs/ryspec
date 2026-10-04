/* ryspec._core: the Python binding of libryspec.
 *
 * lint(path) parses the file at path and lints it, against every rule
 * libryspec checks, in order, 0 being the schema. It returns its findings
 * as a list of (kind, line, column, message) tuples: empty when there are
 * none, else the first, which is the reason the file failed to parse or
 * its first violation.
 * library_version is the version of the libryspec it was built from, as
 * ryspec_version() gives it: MAJOR * 100000 + MINOR * 100 + PATCH. */
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "ryspec/ryspec.h"

static const char* kind_name(int status)
{
  switch(status) {
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

static PyObject* lint(PyObject* self, PyObject* args, PyObject* kwargs)
{
  (void)self;
  static char* keywords[] = {"path", NULL};
  PyObject* path_obj;
  if(!PyArg_ParseTupleAndKeywords(
       args, kwargs, "O&", keywords, PyUnicode_FSConverter, &path_obj)) {
    return NULL;
  }

  ryspec_diag diag;
  int status;
  Py_BEGIN_ALLOW_THREADS ryspec_toml_doc* doc =
    ryspec_toml_parse_file(PyBytes_AS_STRING(path_obj), &diag);
  status = doc ? ryspec_toml_lint(doc, &diag) : diag.status;
  ryspec_toml_doc_free(doc);
  Py_END_ALLOW_THREADS Py_DECREF(path_obj);

  if(status == RYSPEC_OK) {
    return PyList_New(0);
  }
  return Py_BuildValue(
    "[(siis)]", kind_name(status), diag.line, diag.column, diag.message);
}

static PyMethodDef methods[] = {
  {"lint",
   (PyCFunction)(void (*)(void))lint,
   METH_VARARGS | METH_KEYWORDS,
   "lint(path) -> list of (kind, line, column, message)\n\n"
   "Parse and lint the ryspec document at path, against every rule\n"
   "libryspec checks. The list is empty, or holds the first finding."},
  {NULL, NULL, 0, NULL},
};

static struct PyModuleDef module = {
  PyModuleDef_HEAD_INIT,
  "ryspec._core",
  "The libryspec binding.",
  -1,
  methods,
};

PyMODINIT_FUNC PyInit__core(void)
{
  PyObject* m = PyModule_Create(&module);
  if(m && PyModule_AddIntConstant(m, "library_version", ryspec_version()) < 0) {
    Py_CLEAR(m);
  }
  return m;
}
