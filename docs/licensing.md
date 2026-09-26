# Licensing

Short version: **this repository is MIT. The engines it links are not, and the
one that matters most is SpiderMonkey, which is MPL-2.0.** Linking it into a
proprietary product is allowed and always was - what MPL asks for is that the
*engine's* source stays available, not yours. **CPython is permissive** (the PSF
license), but a program linked with that backend carries seven more libraries
and ships the standard library's source beside it, and each wants its notice;
and because the CPython here is patched, the PSF license asks for a summary of
the changes.

This is a description of the position, not legal advice. If you are shipping a
product, have your own counsel read it.

## What is in this repository

No engine. The tree is `include/unibind/`, `src/backends/`, `tests/`, `tools/`,
`examples/`, `docs/` and the build files, written here; `git ls-files` lists no
engine header and no engine binary. `dependencies/` is where the build unpacks a
JavaScript engine it fetched from that engine's own published release at
configure time, and it is ignored by git - `dependencies/README.md` is the only
tracked file under it, and it is prose.

**One directory is not the authors' own**, and it is the exception worth
knowing: `cmake/vcpkg-ports/python3/` is vcpkg's `python3` port, copied and
changed. Its `portfile.cmake`, `vcpkg.json` and the rest are vcpkg's, under
vcpkg's MIT licence (Microsoft); its `.patch` files - vcpkg's and the four
added here - are diffs against CPython's source, and quote lines of it as
context, under the PSF license. Nothing else of CPython is in the tree: vcpkg
downloads the source and builds it at configure time.

Fetching is not distributing, and it does not change any of what follows: the
bytes come from their publisher to the machine that asked for them, and the
obligations below attach to whoever ships a binary built from them.

Build-time third parties:

| | | |
|---|---|---|
| doctest | MIT | vcpkg, tests only, not part of the library |
| V8 | BSD-3-Clause | not vendored; fetched, or a path you point CMake at |
| SpiderMonkey | MPL-2.0 | not vendored; fetched, or a path you point CMake at |
| CPython 3.12.13 | PSF License v2 (and the older CNRI and BeOpen terms it carries) | built by vcpkg; the port and patches in `cmake/vcpkg-ports/python3` |
| the port files themselves | MIT (vcpkg) | `cmake/vcpkg-ports/python3` |
| OpenSSL 3 | Apache-2.0 | built by vcpkg for the python backend; linked into it |
| libffi | MIT | the same |
| SQLite | public domain | the same |
| expat | MIT | the same |
| liblzma (xz) | 0BSD | the same |
| bzip2 | bzip2's BSD-style licence | the same |
| zlib | zlib licence | the same |

So the copyright in this tree is the authors' own, apart from that one port
directory, and the licence here could have been anything. It is MIT.

## Why MIT

The library's whole purpose is to be linked into something else, so the licence
should be the one that gets in the way of the fewest of those somethings.

- **MIT** - shortest, no patent clause, compatible with everything including
  GPLv2, and fine as the non-MPL half of an MPL "Larger Work". Chosen.
- **BSD-3-Clause** - materially the same, and would match V8's. No reason to
  prefer it beyond symmetry.
- **Apache-2.0** - the real alternative. It adds an express patent grant and
  patent-retaliation clause, which is worth something for a large contributor
  base, and costs GPLv2 compatibility. This is a small header-shaped library
  over other people's engines; there is no patent surface here worth buying,
  and GPLv2 projects are a real class of consumer. Not chosen, but it is the
  one to revisit if the project ever grows a contributor policy.

Nothing about MIT conflicts with either engine: a permissive licence is
compatible with BSD-3-Clause by inspection, and MPL-2.0 explicitly contemplates
being combined with files under other terms (§1.7, §3.3).

## What a consumer who ships a binary owes

You are distributing an executable that contains your code, `unibind`, and an
engine. The obligations attach to the engine, and they differ.

### If you built the V8 backend

BSD-3-Clause. Reproduce V8's copyright notice, the licence text and the
disclaimer somewhere the recipient can read it - the "about" box, a
`THIRD-PARTY-NOTICES` file, the documentation. No source disclosure, no
restriction on how you licence your own work.

One thing to check rather than assume: V8's tree bundles third-party
components with their own licences (ICU, zlib, fdlibm and others). They ride
along in a monolith build, so the notice you ship should come from the V8
checkout's own licence files, not from the one-line summary above.

### If you built the SpiderMonkey backend

MPL-2.0, and this is the half worth reading carefully, because the folklore
about Mozilla code is mostly wrong in both directions.

**MPL-2.0 is file-level copyleft.** The unit of obligation is the *file*, not
the linked program. §1.7 and §3.3 say it directly: you may combine Covered
Software with files under other terms into a "Larger Work" and licence the
Larger Work however you like, provided the Covered Software itself stays under
MPL.

In practice, shipping a product that statically links `spidermonkey.lib`:

1. **Your own source stays yours.** `unibind` is MIT, your application is whatever
   you say it is, and neither becomes MPL by being linked with the engine.
   There is no LGPL-style relinking requirement either - static linking is
   fine, which is the single most common misconception about MPL.
2. **The engine's source has to be available to your recipients**, under
   MPL-2.0, for the exact version you shipped (§3.2). Telling them where to get
   it is enough - "SpiderMonkey 153.3.0esr, from
   <https://archive.mozilla.org/pub/firefox/releases/>" plus any patches of
   yours - and it may be given "at a charge no more than the cost of
   distribution".
3. **If you modify an MPL file, that file stays MPL and its modified source
   must be published.** Changing SpiderMonkey and keeping the change private is
   the one thing MPL does not allow. `unibind` never modifies the engine, so this
   only bites if you rebuild it with patches.
4. **Keep the notices** (§3.4): the licence text and the attribution that came
   with the engine go into whatever notice file you ship.
5. **The build you actually link is more than MPL-2.0.** A SpiderMonkey static
   library pulls in mozglue, ICU, Rust crates and other vendored code, some of
   it Apache-2.0 or BSD or Unicode-licensed. None of it is copyleft beyond MPL,
   but the notice file should be generated from the build you shipped rather
   than from this page.

**The practical consequence, in one sentence:** shipping a closed-source
product on top of the SpiderMonkey backend is fine, and costs you a notice file
and a URL that keeps working.

### If you built the CPython backend

Everything here is permissive - no copyleft anywhere - and a closed-source
product is fine. What it costs is notices, and one thing the PSF license asks
that the others do not. Two parts of CPython go out with your program, and both
are covered:

- **The engine, linked in**: `python312.lib`, which is CPython with the standard
  library's C extension modules built into it.
- **The standard library, beside it**: the `Lib/` directory a program has to ship
  (`docs/python.md` section 10) is CPython's own Python source, distributed as
  source.

What you owe:

1. **The PSF license text and its copyright notice**, "Copyright (c) 2001 Python
   Software Foundation; All Rights Reserved", retained with the software (§2).
   vcpkg installs the text as `share/python3/copyright` in the prefix it built;
   that is CPython's `LICENSE` file, history and the CNRI and BeOpen terms
   included.
2. **A brief summary of the changes** (§3). The PSF license asks it of anyone who
   makes a derivative work available, and a CPython built with patches is one:
   the registry port's patches and the four added here
   (`cmake/vcpkg-ports/README.md` describes ours, each with its reason, and the
   port's other patches are named in its `portfile.cmake`). Shipping that README,
   or a paragraph made from it, discharges this.
3. **The notices of what CPython incorporates.** CPython's own source contains
   third-party code under other permissive terms - libmpdec, which `_decimal` is
   built on, and others - listed in CPython's documentation, "Licenses and
   Acknowledgements for Incorporated Software", for the version you ship. vcpkg's
   `copyright` file does not include that list; take it from the documentation of
   3.12.13.
4. **The seven libraries linked beside it**, each from its own `copyright` file
   under `share/<port>/` in the same vcpkg prefix:
   - **OpenSSL 3, Apache-2.0**: include the licence text; §4 also asks that a
     `NOTICE` file, where the work has one, be passed on.
   - **libffi, MIT** and **expat, MIT**: the copyright notice and permission
     notice.
   - **bzip2** and **zlib**: nothing is required in a binary distribution - both
     ask only that source be kept with its notice and altered source be marked -
     and an acknowledgement in the documentation is appreciated.
   - **liblzma (xz), 0BSD**: nothing is required.
   - **SQLite, public domain**: nothing is required.

   Only the ones actually linked apply, and a program built from this tree links
   all seven: `_unibind_provide_python` names whichever the prefix holds.

None of it asks for your source, and none of it restricts how you licence your
own work.

### Either way

`unibind` itself adds one line to your notice file - the MIT text above with its
copyright line - and nothing else.

## What this repository deliberately does not do

- **It ships no engine binary and no engine source**, so it triggers none of
  the obligations above by itself. That is a licensing property as much as a
  practical one, and it is why an engine is fetched into `dependencies/`, or
  built by vcpkg, rather than vendored into the tree. The one qualification is
  the CPython port's patch files, which quote CPython's source as diff context
  under its own licence.
- **It expresses no opinion on which engine you should ship.** The licences
  differ, the obligations differ, and that is now a thing you can decide at
  build time with `-DUNIBIND_BACKEND=`.
