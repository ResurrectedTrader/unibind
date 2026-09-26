// The pure-Python standard library, compiled into the backend.
//
// With UNIBIND_PYTHON_EMBED_STDLIB on (the default), the build runs
// freeze_stdlib.py with the target CPython over its `Lib` directory, and the
// source it generates defines `kTable`: every module's marshalled code object,
// `#embed`-ed, indexed by CPython's own frozen-module table. `Platform` points
// `PyImport_FrozenModules` at it when no standard library directory is given,
// and CPython's FrozenImporter - ahead of the path finder on `sys.meta_path` -
// serves every `import` from it. See docs/python.md, "Where the standard
// library comes from".

#pragma once

#include <cstddef>

struct _frozen;

namespace ub::detail::frozen_stdlib {

struct Table {
    /// CPython's frozen-module table, ended by an entry whose name is null.
    const _frozen* modules;
    /// How many modules, not counting the end.
    std::size_t count;
    /// The bytes of marshalled code the modules point into.
    std::size_t bytes;
};

extern const Table kTable;

}  // namespace ub::detail::frozen_stdlib
