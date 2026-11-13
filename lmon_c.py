#!/usr/bin/env python3
"""LMON (Light Machine / Lucian Marin Object Notation) — C-accelerated Python implementation.

Wraps the single-file `lmon.c` C11 engine via the CPython C API (auto-compiled and
cached in `__pycache__/` on first import with zero external build dependencies) while
exposing the exact same Pythonic API and CLI as `lmon.py`:

Library API:
    loads(text: str) -> Any
    load(fp) -> Any
    dumps(obj: Any) -> str
    dump(obj: Any, fp) -> None
    LMONDecodeError(ValueError)

CLI usage:
    python3 lmon_c.py <file.lmon>            # Parse LMON and print formatted JSON
    python3 lmon_c.py --compact <file.lmon>  # Parse LMON and print compact JSON
    python3 lmon_c.py --lmon <file>          # Output canonical LMON (from .lmon or .json)
    python3 lmon_c.py --from-json <file>     # Convert JSON file to LMON
    python3 lmon_c.py --check <file.lmon>    # Validate LMON syntax only
    python3 lmon_c.py --test                 # Run conformance test suite
    python3 lmon_c.py --bench [file.lmon]    # Run LMON (C ext) vs stdlib json benchmark
"""

from __future__ import annotations

import hashlib
import importlib.machinery
import importlib.util
import json
import os
import shlex
import subprocess
import sys
import sysconfig
import time
from pathlib import Path
from typing import Any, IO, List, Optional, Tuple

__all__ = ["LMONDecodeError", "loads", "load", "dumps", "dump"]


def _compute_line_col(doc: str, pos: int) -> Tuple[int, int]:
    line = 1
    col = 1
    i = 0
    end = min(pos, len(doc))
    while i < end:
        ch = doc[i]
        if ch == "\n":
            line += 1
            col = 1
        elif ch == "\r":
            line += 1
            col = 1
            if i + 1 < end and doc[i + 1] == "\n":
                i += 1
        else:
            col += 1
        i += 1
    return line, col


class LMONDecodeError(ValueError):
    """Raised when an LMON document violates the LMON 1.0 specification."""

    def __init__(self, msg: str, doc: str, pos: int) -> None:
        # Convert UTF-8 byte offset from C parser to Python char index
        raw_prefix = doc.encode("utf-8")[:pos]
        char_pos = len(raw_prefix.decode("utf-8", errors="replace"))
        line, col = _compute_line_col(doc, char_pos)
        super().__init__(f"{msg}: line {line} column {col} (char {char_pos})")
        self.msg = msg
        self.doc = doc
        self.pos = char_pos
        self.lineno = line
        self.colno = col


_C_EXTENSION_SOURCE = r"""
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#define LMON_NO_MAIN
#include "lmon.c"

#define KEY_CACHE_SIZE 128u
#define KEY_CACHE_MASK (KEY_CACHE_SIZE - 1u)
#define MAX_DUMP_DEPTH 1000

typedef struct {
    const char *ptr;
    size_t len;
    PyObject *py_str;
} KeyCacheSlot;

typedef struct {
    KeyCacheSlot slots[KEY_CACHE_SIZE];
} KeyCache;

static inline uint32_t fnv1a_key(const char *s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

static inline PyObject *key_cache_get(KeyCache *kc, LmonStr k) {
    if (k.len <= 64) {
        uint32_t idx = fnv1a_key(k.ptr, k.len) & KEY_CACHE_MASK;
        KeyCacheSlot *slot = &kc->slots[idx];
        if (slot->py_str != NULL && slot->len == k.len &&
            memcmp(slot->ptr, k.ptr, k.len) == 0) {
            Py_INCREF(slot->py_str);
            return slot->py_str;
        }
        PyObject *s = PyUnicode_FromStringAndSize(k.ptr, (Py_ssize_t)k.len);
        if (!s) return NULL;
        Py_XDECREF(slot->py_str);
        slot->ptr = k.ptr;
        slot->len = k.len;
        slot->py_str = s;
        Py_INCREF(s);
        return s;
    }
    return PyUnicode_FromStringAndSize(k.ptr, (Py_ssize_t)k.len);
}

static inline void key_cache_clear(KeyCache *kc) {
    for (size_t i = 0; i < KEY_CACHE_SIZE; i++) {
        Py_XDECREF(kc->slots[i].py_str);
    }
}

static PyObject *lmon_val_to_py(const LmonValue *v, KeyCache *kc) {
    switch (v->type) {
        case LMON_NULL:
            Py_INCREF(Py_None);
            return Py_None;
        case LMON_BOOL: {
            PyObject *b = v->u.b ? Py_True : Py_False;
            Py_INCREF(b);
            return b;
        }
        case LMON_INT:
            return PyLong_FromLongLong((long long)v->u.i);
        case LMON_FLOAT:
            return PyFloat_FromDouble(v->u.f);
        case LMON_STRING:
            return PyUnicode_FromStringAndSize(v->u.s.ptr, (Py_ssize_t)v->u.s.len);
        case LMON_ARRAY: {
            Py_ssize_t n = (Py_ssize_t)v->u.arr.len;
            PyObject *lst = PyList_New(n);
            if (!lst) return NULL;
            for (Py_ssize_t i = 0; i < n; i++) {
                PyObject *item = lmon_val_to_py(&v->u.arr.items[i], kc);
                if (!item) {
                    Py_DECREF(lst);
                    return NULL;
                }
                PyList_SET_ITEM(lst, i, item);
            }
            return lst;
        }
        case LMON_OBJECT: {
            Py_ssize_t n = (Py_ssize_t)v->u.obj.len;
            PyObject *dct = _PyDict_NewPresized(n);
            if (!dct) return NULL;
            for (Py_ssize_t i = 0; i < n; i++) {
                PyObject *k = key_cache_get(kc, v->u.obj.members[i].key);
                if (!k) {
                    Py_DECREF(dct);
                    return NULL;
                }
                PyObject *val = lmon_val_to_py(&v->u.obj.members[i].val, kc);
                if (!val) {
                    Py_DECREF(k);
                    Py_DECREF(dct);
                    return NULL;
                }
                int rc = PyDict_SetItem(dct, k, val);
                Py_DECREF(k);
                Py_DECREF(val);
                if (rc < 0) {
                    Py_DECREF(dct);
                    return NULL;
                }
            }
            return dct;
        }
    }
    Py_RETURN_NONE;
}

typedef struct {
    PyObject *decode_error_cls;
    LmonArena arena;
    bool arena_initialized;
} LmonModState;

static LmonModState g_state = {NULL, {NULL, 0}, false};

static PyObject *py_lmon_loads(PyObject *Py_UNUSED(self), PyObject *arg) {
    if (!PyUnicode_Check(arg)) {
        PyErr_Format(PyExc_TypeError,
                     "the LMON object must be str, not %.200s",
                     Py_TYPE(arg)->tp_name);
        return NULL;
    }

    Py_ssize_t len = 0;
    const char *src = PyUnicode_AsUTF8AndSize(arg, &len);
    if (!src) return NULL;

    /* Reject embedded NUL bytes cleanly since C strings/UTF-8 slices forbid unescaped 0x00 */
    if (memchr(src, '\0', (size_t)len) != NULL) {
        size_t nul_pos = (size_t)((const char *)memchr(src, '\0', (size_t)len) - src);
        PyObject *exc = PyObject_CallFunction(
            g_state.decode_error_cls, "sOn",
            "unescaped NUL character is not allowed", arg, (Py_ssize_t)nul_pos
        );
        if (exc) {
            PyErr_SetObject(g_state.decode_error_cls, exc);
            Py_DECREF(exc);
        }
        return NULL;
    }

    if (!g_state.arena_initialized) {
        lmon_arena_init(&g_state.arena, 65536);
        g_state.arena_initialized = true;
    } else {
        lmon_arena_reset(&g_state.arena);
    }

    LmonError err;
    LmonValue *root = lmon_parse(src, (size_t)len, &g_state.arena, &err);
    if (!root) {
        PyObject *exc = PyObject_CallFunction(
            g_state.decode_error_cls, "sOn",
            err.message ? err.message : "LMON parse error",
            arg,
            (Py_ssize_t)err.pos
        );
        if (exc) {
            PyErr_SetObject(g_state.decode_error_cls, exc);
            Py_DECREF(exc);
        }
        return NULL;
    }

    KeyCache kc;
    memset(&kc, 0, sizeof(kc));
    PyObject *result = lmon_val_to_py(root, &kc);
    key_cache_clear(&kc);

    /* Release oversized arena blocks (> 4 MiB) so large one-off parses don't stay resident */
    if (g_state.arena.head && g_state.arena.head->cap > (4u * 1024u * 1024u)) {
        lmon_arena_free(&g_state.arena);
        g_state.arena_initialized = false;
    }

    return result;
}

/* Native C Python-object -> Canonical LMON serializer */
static int py_try_write_inline(LmonBuf *b, PyObject *v, int depth) {
    if (depth > MAX_DUMP_DEPTH) {
        PyErr_SetString(PyExc_RecursionError, "maximum recursion depth exceeded while encoding LMON");
        return -1;
    }
    if (v == Py_None) {
        lmon_buf_Append(b, "null", 4);
        return 1;
    }
    if (v == Py_True) {
        lmon_buf_Append(b, "true", 4);
        return 1;
    }
    if (v == Py_False) {
        lmon_buf_Append(b, "false", 5);
        return 1;
    }
    if (PyUnicode_Check(v)) {
        Py_ssize_t slen = 0;
        const char *sptr = PyUnicode_AsUTF8AndSize(v, &slen);
        if (!sptr) return -1;
        LmonStr ls = {sptr, (size_t)slen};
        lmon_write_escaped_str(b, ls);
        return 1;
    }
    if (PyLong_Check(v)) {
        int overflow = 0;
        long long iv = PyLong_AsLongLongAndOverflow(v, &overflow);
        if (overflow == 0 && !PyErr_Occurred()) {
            char num[32];
            int n = snprintf(num, sizeof(num), "%lld", iv);
            if (n > 0) lmon_buf_Append(b, num, (size_t)n);
            return 1;
        }
        PyErr_Clear();
        PyObject *s_obj = PyObject_Str(v);
        if (!s_obj) return -1;
        Py_ssize_t slen = 0;
        const char *sptr = PyUnicode_AsUTF8AndSize(s_obj, &slen);
        if (!sptr) { Py_DECREF(s_obj); return -1; }
        lmon_buf_Append(b, sptr, (size_t)slen);
        Py_DECREF(s_obj);
        return 1;
    }
    if (PyFloat_Check(v)) {
        double fv = PyFloat_AsDouble(v);
        if (fv == -1.0 && PyErr_Occurred()) return -1;
        if (isnan(fv) || isinf(fv)) {
            PyErr_SetString(PyExc_ValueError,
                            "Out of range float values (NaN/Infinity) are not LMON compliant");
            return -1;
        }
        lmon_write_float(b, fv);
        return 1;
    }
    if (PyDict_Check(v)) {
        if (PyDict_Size(v) == 0) {
            lmon_buf_Append(b, "{}", 2);
            return 1;
        }
        return 0;
    }
    if (PyList_Check(v) || PyTuple_Check(v)) {
        Py_ssize_t n = PyList_Check(v) ? PyList_GET_SIZE(v) : PyTuple_GET_SIZE(v);
        if (n == 0) {
            lmon_buf_Append(b, "[]", 2);
            return 1;
        }
        size_t start_len = b->len;
        lmon_buf_putc(b, '[');
        for (Py_ssize_t i = 0; i < n; i++) {
            if (i > 0) lmon_buf_Append(b, ", ", 2);
            PyObject *item = PyList_Check(v) ? PyList_GET_ITEM(v, i) : PyTuple_GET_ITEM(v, i);
            int ok = py_try_write_inline(b, item, depth + 1);
            if (ok < 0) return -1;
            if (ok == 0 || (b->len - start_len + 1 > 80)) {
                b->len = start_len;
                b->data[b->len] = '\0';
                return 0;
            }
        }
        lmon_buf_putc(b, ']');
        if (b->len - start_len > 80) {
            b->len = start_len;
            b->data[b->len] = '\0';
            return 0;
        }
        return 1;
    }

    PyErr_Format(PyExc_TypeError,
                 "Object of type %.200s is not LMON serializable",
                 Py_TYPE(v)->tp_name);
    return -1;
}

static int py_emit_block(LmonBuf *b, PyObject *v, size_t indent_level, int depth, const char *unit, size_t unit_len) {
    if (depth > MAX_DUMP_DEPTH) {
        PyErr_SetString(PyExc_RecursionError, "maximum recursion depth exceeded while encoding LMON");
        return -1;
    }
    if (PyDict_Check(v)) {
        Py_ssize_t pos = 0;
        PyObject *key = NULL;
        PyObject *val = NULL;
        while (PyDict_Next(v, &pos, &key, &val)) {
            if (!PyUnicode_Check(key)) {
                PyErr_Format(PyExc_TypeError,
                             "keys must be str, not %.200s",
                             Py_TYPE(key)->tp_name);
                return -1;
            }
            Py_ssize_t klen = 0;
            const char *kptr = PyUnicode_AsUTF8AndSize(key, &klen);
            if (!kptr) return -1;
            LmonStr ks = {kptr, (size_t)klen};

            lmon_buf_repeat_unit(b, unit, unit_len, indent_level);
            if (lmon_key_can_be_unquoted(ks)) {
                lmon_buf_Append(b, ks.ptr, ks.len);
            } else {
                lmon_write_escaped_str(b, ks);
            }
            lmon_buf_putc(b, ':');

            size_t colon_pos = b->len;
            lmon_buf_putc(b, ' ');
            int ok = py_try_write_inline(b, val, depth + 1);
            if (ok < 0) return -1;
            if (ok == 1) {
                lmon_buf_putc(b, '\n');
            } else {
                b->len = colon_pos;
                b->data[b->len] = '\0';
                lmon_buf_putc(b, '\n');
                size_t child_indent = (PyList_Check(val) || PyTuple_Check(val)) ? indent_level : indent_level + 1;
                if (py_emit_block(b, val, child_indent, depth + 1, unit, unit_len) < 0) return -1;
            }
        }
        return 0;
    }
    if (PyList_Check(v) || PyTuple_Check(v)) {
        Py_ssize_t n = PyList_Check(v) ? PyList_GET_SIZE(v) : PyTuple_GET_SIZE(v);
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject *item = PyList_Check(v) ? PyList_GET_ITEM(v, i) : PyTuple_GET_ITEM(v, i);
            lmon_buf_repeat_unit(b, unit, unit_len, indent_level);
            lmon_buf_putc(b, '-');

            size_t dash_pos = b->len;
            lmon_buf_putc(b, ' ');
            int ok = py_try_write_inline(b, item, depth + 1);
            if (ok < 0) return -1;
            if (ok == 1) {
                lmon_buf_putc(b, '\n');
            } else {
                b->len = dash_pos;
                b->data[b->len] = '\0';
                lmon_buf_putc(b, '\n');
                if (py_emit_block(b, item, indent_level + 1, depth + 1, unit, unit_len) < 0) return -1;
            }
        }
        return 0;
    }

    PyErr_Format(PyExc_TypeError,
                 "Object of type %.200s is not LMON serializable",
                 Py_TYPE(v)->tp_name);
    return -1;
}

static PyObject *py_lmon_dumps(PyObject *Py_UNUSED(self), PyObject *args) {
    PyObject *arg = NULL;
    const char *unit = "    ";
    Py_ssize_t unit_slen = 4;
    if (!PyArg_ParseTuple(args, "O|s#", &arg, &unit, &unit_slen)) {
        return NULL;
    }
    size_t unit_len = 0;
    if (!lmon_valid_indent_unit(unit, &unit_len)) {
        PyErr_SetString(PyExc_ValueError, "indent must be a positive integer (spaces) or a non-empty string of spaces or tabs");
        return NULL;
    }

    LmonBuf b;
    if (!lmon_buf_init(&b, 512)) return PyErr_NoMemory();

    int inline_ok = py_try_write_inline(&b, arg, 0);
    if (inline_ok < 0) {
        free(b.data);
        return NULL;
    }
    if (inline_ok == 1) {
        lmon_buf_putc(&b, '\n');
    } else {
        if (py_emit_block(&b, arg, 0, 0, unit, unit_len) < 0) {
            free(b.data);
            return NULL;
        }
    }

    PyObject *res = PyUnicode_FromStringAndSize(b.data, (Py_ssize_t)b.len);
    free(b.data);
    return res;
}

static PyObject *py_lmon_set_error_cls(PyObject *Py_UNUSED(self), PyObject *cls) {
    Py_INCREF(cls);
    Py_XDECREF(g_state.decode_error_cls);
    g_state.decode_error_cls = cls;
    Py_RETURN_NONE;
}

static void lmon_ext_free(void *Py_UNUSED(mod)) {
    Py_CLEAR(g_state.decode_error_cls);
    if (g_state.arena_initialized) {
        lmon_arena_free(&g_state.arena);
        g_state.arena_initialized = false;
    }
}

static PyMethodDef lmon_methods[] = {
    {"loads", py_lmon_loads, METH_O, "Parse an LMON string into Python objects using lmon.c."},
    {"dumps", py_lmon_dumps, METH_VARARGS, "Serialize Python objects into canonical LMON using lmon.c."},
    {"_set_error_cls", py_lmon_set_error_cls, METH_O, "Register LMONDecodeError class."},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef lmon_module = {
    PyModuleDef_HEAD_INIT,
    "_lmon_c_ext",
    "Native C LMON bindings powered by lmon.c",
    -1,
    lmon_methods,
    NULL,
    NULL,
    NULL,
    lmon_ext_free
};

PyMODINIT_FUNC PyInit__lmon_c_ext(void) {
    return PyModule_Create(&lmon_module);
}
"""


def _load_or_build_extension():
    here = Path(__file__).resolve().parent
    lmon_c_path = here / "lmon.c"
    if not lmon_c_path.is_file():
        raise RuntimeError(f"Required C source file not found: {lmon_c_path}")

    cache_dir = here / "__pycache__"
    cache_dir.mkdir(exist_ok=True)

    ext_suffix = sysconfig.get_config_var("EXT_SUFFIX") or ".so"
    digest_input = (
        lmon_c_path.read_bytes()
        + _C_EXTENSION_SOURCE.encode("utf-8")
        + f"{sys.version_info.major}.{sys.version_info.minor}:{ext_suffix}".encode("utf-8")
    )
    tag = hashlib.sha256(digest_input).hexdigest()[:16]
    so_path = cache_dir / f"_lmon_c_ext_{tag}{ext_suffix}"

    if not so_path.is_file():
        include_dir = sysconfig.get_path("include")
        if not include_dir or not Path(include_dir, "Python.h").is_file():
            raise RuntimeError(f"Python C headers (Python.h) not found in {include_dir!r}")

        cc_var = os.environ.get("CC") or sysconfig.get_config_var("CC") or "cc"
        cc_cmd = shlex.split(cc_var)

        c_src_path = cache_dir / f"_lmon_c_ext_{tag}_{os.getpid()}.c"
        tmp_so_path = cache_dir / f"_lmon_c_ext_{tag}_{os.getpid()}{ext_suffix}"

        c_src_path.write_text(_C_EXTENSION_SOURCE, encoding="utf-8")
        try:
            cmd = [
                *cc_cmd,
                "-O3",
                "-std=c11",
                "-shared",
                "-fPIC",
                f"-I{here}",
                f"-I{include_dir}",
            ]
            if sys.platform == "darwin":
                cmd.extend(["-undefined", "dynamic_lookup"])
            cmd.extend([str(c_src_path), "-o", str(tmp_so_path)])

            proc = subprocess.run(cmd, capture_output=True, text=True)
            if proc.returncode != 0:
                raise RuntimeError(
                    f"Failed to compile _lmon_c_ext ({' '.join(cmd)}):\n{proc.stderr}"
                )
            os.replace(tmp_so_path, so_path)
        finally:
            if c_src_path.exists():
                c_src_path.unlink()
            if tmp_so_path.exists():
                tmp_so_path.unlink()

    loader = importlib.machinery.ExtensionFileLoader("_lmon_c_ext", str(so_path))
    spec = importlib.util.spec_from_loader("_lmon_c_ext", loader)
    if spec is None:
        raise RuntimeError("Could not create module spec for _lmon_c_ext")
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    module._set_error_cls(LMONDecodeError)
    return module


_ext = _load_or_build_extension()


def _normalize_indent(indent: Any) -> str:
    if isinstance(indent, int) and not isinstance(indent, bool):
        if indent >= 1:
            return " " * indent
    elif isinstance(indent, str) and len(indent) >= 1:
        if indent.strip(" ") == "" or indent.strip("\t") == "":
            return indent
    raise ValueError("indent must be a positive integer (spaces) or a non-empty string of spaces or tabs")


def loads(text: str) -> Any:
    """Parse an LMON string into native Python objects using `lmon.c`."""
    return _ext.loads(text)


def load(fp: IO[str]) -> Any:
    """Parse an LMON text stream into native Python objects using `lmon.c`."""
    return _ext.loads(fp.read())


def dumps(obj: Any, indent: Any = 4) -> str:
    """Serialize a Python JSON-compatible value into LMON text using `lmon.c`."""
    return _ext.dumps(obj, _normalize_indent(indent))


def dump(obj: Any, fp: IO[str], indent: Any = 4) -> None:
    """Serialize a Python JSON-compatible value to a text stream as LMON."""
    fp.write(_ext.dumps(obj, _normalize_indent(indent)))


# ---------------------------------------------------------------------------
# Conformance Self-Tests & CLI
# ---------------------------------------------------------------------------


def _run_tests() -> int:
    cases: List[Tuple[str, Any]] = [
        ("empty_obj", {}),
        ("empty_arr", []),
        ("scalars", {"s": "Lemon", "i": -42, "f": 3.25, "t": True, "fa": False, "n": None}),
        ("root_int", -99),
        ("root_big_int", 10**30),
        ("root_str", "hello: world"),
        ("root_bool", True),
        ("root_null", None),
        (
            "nested_mix",
            {
                "service": "lmon",
                "tags": ["a", "b", "c"],
                "empty_map": {},
                "empty_list": [],
                "matrix": [[1, 2], [3, 4]],
                "nodes": [
                    {"id": 1, "meta": {"ok": True}},
                    {"id": 2, "meta": {}},
                ],
                "": "empty",
                "true": "keyword",
                "false": False,
                "null": None,
                "123": "numeric-key",
                "-dash": "dash-key",
                "with space": "quoted\nline\t🍋",
            },
        ),
    ]

    indents = (1, 2, 3, 4, 5, 6, 7, 8, "\t", "\t\t")
    for name, expected in cases:
        for ind in indents:
            encoded = dumps(expected, indent=ind)
            decoded = loads(encoded)
            if name == "root_big_int":
                if not isinstance(decoded, float):
                    print(f"FAIL [{name}]: expected float fallback for >64-bit int", file=sys.stderr)
                    return 1
                continue
            if decoded != expected:
                print(f"FAIL [{name} indent={ind!r}]: round-trip mismatch", file=sys.stderr)
                return 1

    if loads('emoji: "\\uD83C\\uDF4B"\n') != {"emoji": "🍋"}:
        print("FAIL [surrogate_pair]", file=sys.stderr)
        return 1

    if loads("a:\r\n\tb: 1\rc:\r-\r\n\tx: true\r- 2\r") != {"a": {"b": 1}, "c": [{"x": True}, 2]}:
        print("FAIL [universal_newlines]", file=sys.stderr)
        return 1

    invalid_inputs = [
        "",
        "   \n",
        "   bad_indent_root: 1\n",
        "\tbad_indent_root_tab: 1\n",
        "a:\n    b: 1\n  c: 2\n",
        "a:\n   b: 1\n     c: 2\n",
        "a:\n  b:\n      c: 1\n",
        "a:\n\tb:\n\t\t\tc: 1\n",
        "a:\n\t\tb: 1\n\tc: 2\n",
        "a:\n  b: 1\n\tc: 2\n",
        "a:\n\tb: 1\n  c: 2\n",
        "a:\n \tb: 1\n",
        "a:\n    - 1\n",
        "dangling:\n",
        "dangling:\nstill_root: 1\n",
        'curly: {"a": 1}\n',
        "true: 1\n",
        "trailing_space: 1 \n",
        "two_spaces:  1\n",
        "bad_num: 01\n",
        "trailing_comma: [1, 2,]\n",
        "mixed: 1\n- 2\n",
        "# comments are forbidden\na: 1\n",
    ]
    for bad in invalid_inputs:
        try:
            loads(bad)
            print(f"FAIL [invalid accepted]: {bad!r}", file=sys.stderr)
            return 1
        except LMONDecodeError:
            pass

    print(
        f"Python C-accelerated LMON (lmon_c.py): all "
        f"{len(cases) * len(indents) + len(invalid_inputs) + 2} conformance tests passed."
    )
    return 0


def _run_bench(path: Optional[str]) -> int:
    if path:
        with open(path, "r", encoding="utf-8") as f:
            sample = f.read()
    else:
        records = [
            {
                "id": i,
                "name": f"service-node-{i}",
                "active": (i % 2 == 0),
                "latency_ms": round(1.25 * (i % 17), 3),
                "tags": ["edge", "lemon", "v1"],
                "meta": {"region": "eu-central", "capacity": 256 + i, "extra": {}},
            }
            for i in range(500)
        ]
        sample = dumps({"cluster": "lmon-bench", "count": 500, "nodes": records})

    parsed = loads(sample)
    sample4 = sample if path else dumps(parsed, indent=4)
    sample_tab = dumps(parsed, indent="\t")
    json_sample = json.dumps(parsed, indent=2) + "\n"

    lmon_bytes = len(sample4.encode("utf-8"))
    tab_bytes = len(sample_tab.encode("utf-8"))
    json_bytes = len(json_sample.encode("utf-8"))
    size_kb = lmon_bytes / 1024.0
    iterations = max(50, int(100000 / max(1.0, size_kb)))

    # Warmup
    for _ in range(10):
        loads(sample4)
        loads(sample_tab)
        json.loads(json_sample)

    t0 = time.perf_counter()
    for _ in range(iterations):
        loads(sample4)
    lmon_sec = time.perf_counter() - t0

    t_tab = time.perf_counter()
    for _ in range(iterations):
        loads(sample_tab)
    tab_sec = time.perf_counter() - t_tab

    t1 = time.perf_counter()
    for _ in range(iterations):
        json.loads(json_sample)
    json_sec = time.perf_counter() - t1

    lmon_mbs = (lmon_bytes * iterations) / (1024.0 * 1024.0 * lmon_sec)
    tab_mbs = (tab_bytes * iterations) / (1024.0 * 1024.0 * tab_sec)
    json_mbs = (json_bytes * iterations) / (1024.0 * 1024.0 * json_sec)
    print(f"Python lmon_c.py vs stdlib json ({iterations} iterations):")
    print(f"  LMON (4-space):     {size_kb:.1f} KiB/doc -> {lmon_sec:.3f}s ({iterations/lmon_sec:.1f} docs/s, {lmon_mbs:.1f} MiB/s)")
    print(f"  LMON (tab):         {tab_bytes/1024.0:.1f} KiB/doc -> {tab_sec:.3f}s ({iterations/tab_sec:.1f} docs/s, {tab_mbs:.1f} MiB/s)")
    print(f"  JSON (stdlib json): {json_bytes/1024.0:.1f} KiB/doc -> {json_sec:.3f}s ({iterations/json_sec:.1f} docs/s, {json_mbs:.1f} MiB/s)")
    print(f"  Result: lmon_c.py parsed {json_sec/lmon_sec:.2f}x (4-space) / {json_sec/tab_sec:.2f}x (tab) faster per document than Python's built-in C json.loads!")
    return 0


def _parse_indent_arg(val: str) -> Any:
    if val in ("tab", "\\t"):
        return "\t"
    if val.isdigit() and int(val) >= 1:
        return int(val)
    if len(val) >= 1 and (val.strip(" ") == "" or val.strip("\t") == ""):
        return val
    raise ValueError("--indent must be a positive integer or tab")


def _usage() -> str:
    return (
        "Usage: python3 lmon_c.py [options] <file.lmon | ->\n"
        "Options:\n"
        "  -c, --compact          Output compact single-line JSON\n"
        "  -l, --lmon             Output canonical LMON\n"
        "  --indent <N|tab>       Set LMON output indentation in spaces or tab (default: 4)\n"
        "  --from-json            Parse input as JSON and output LMON\n"
        "  --check                Validate LMON syntax only\n"
        "  --test                 Run conformance test suite\n"
        "  --bench [file]         Run LMON (C ext) vs stdlib json benchmark\n"
    )


def main(argv: List[str]) -> int:
    if not argv or "-h" in argv or "--help" in argv:
        sys.stdout.write(_usage())
        return 0 if argv else 1

    if argv[0] == "--test":
        return _run_tests()
    if argv[0] == "--bench":
        return _run_bench(argv[1] if len(argv) > 1 else None)

    compact = False
    out_lmon = False
    from_json = False
    check_only = False
    indent_opt: Any = 4
    target: Optional[str] = None

    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg in ("-c", "--compact"):
            compact = True
        elif arg in ("-l", "--lmon"):
            out_lmon = True
        elif arg == "--indent":
            i += 1
            if i >= len(argv):
                sys.stderr.write(_usage())
                return 2
            try:
                indent_opt = _parse_indent_arg(argv[i])
            except ValueError as exc:
                sys.stderr.write(f"lmon error: {exc}\n")
                return 2
            out_lmon = True
        elif arg == "--from-json":
            from_json = True
            out_lmon = True
        elif arg == "--check":
            check_only = True
        elif arg.startswith("-") and arg != "-":
            sys.stderr.write(f"Unknown option: {arg}\n{_usage()}")
            return 2
        else:
            target = arg
        i += 1

    if target is None:
        sys.stderr.write(_usage())
        return 2

    try:
        if target == "-":
            raw = sys.stdin.read()
        else:
            with open(target, "r", encoding="utf-8", newline="") as f:
                raw = f.read()
            if target.endswith(".json"):
                from_json = True

        data = json.loads(raw) if from_json else loads(raw)
        if check_only:
            return 0
        if out_lmon:
            sys.stdout.write(dumps(data, indent=indent_opt))
        elif compact:
            sys.stdout.write(json.dumps(data, ensure_ascii=False, separators=(",", ":")) + "\n")
        else:
            sys.stdout.write(json.dumps(data, ensure_ascii=False, indent=2) + "\n")
        return 0
    except (LMONDecodeError, json.JSONDecodeError, OSError, ValueError) as exc:
        sys.stderr.write(f"lmon error: {exc}\n")
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
