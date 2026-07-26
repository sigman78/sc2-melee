# Removing the vendored regex engine

`sc2/src/regex/` is a 339 KiB copy of glibc's POSIX regex implementation
carried so that two directory listings can match a file extension
case-insensitively. This proposes deleting it.

Status: **done**, except the optional glob cleanup of §4, which is still
open. Verified at runtime: all three `.rmp` resource indices still load and
the startup log is byte-identical to before; and a two-sided check of the
archive filter (`lower.uqm`, `UPPER.ZIP`, `MiXeD.Uqm` selected;
`ignoreme.txt`, `notanextension` skipped) matches the old regex exactly.

## 1. What the tree actually uses regex for

The whole dependency chain is four files deep and ends at two call sites.

`src/libs/uio/match.h:30` unconditionally does `#define HAVE_REGEX`, which
switches on `match_MATCH_REGEX` in uio's pattern matcher. The matcher's regex
backend (`match.c:487-568`, ~80 lines) is the only consumer of `<regex.h>`:
`regcomp`, `regexec`, `regerror`, `regfree`, and one `regex_t` member.

`match_MATCH_REGEX` is passed by exactly two callers, both in `src/options.c`,
both with a compile-time constant pattern:

| Call site | Pattern | Means |
|---|---|---|
| `options.c:469` (`mountDirZips`) | `\.([zZ][iI][pP]\|[uU][qQ][mM])$` | case-insensitive `.zip` or `.uqm` suffix |
| `options.c:496` (`loadIndices`) | `\.[rR][mM][pP]$` | case-insensitive `.rmp` suffix |

Every other caller of `uio_getDirList` uses `match_MATCH_LITERAL`,
`_PREFIX`, `_SUFFIX` or `_SUBSTRING`:
`options.c:404` (PREFIX), `supermelee/loadmele.c:454` (SUFFIX, `.mle`),
`libs/graphics/gfxload.c:408` (SUBSTRING, `.`),
`libs/resource/direct.c:39` (forwards its caller's type; the only two
callers are the two above). `libs/cdp/cdp.c:385` uses SUFFIX but that
directory is not built at all.

So: no regular expression in this codebase needs anything a suffix
comparison cannot do.

## 2. Cost of keeping it

- 339 KiB / 8 files of third-party C that has to compile, and that shows up
  in every warning sweep and every future port.
- A configure-time probe (`check_symbol_exists(regcomp "regex.h" …)`) plus a
  conditional target, a conditional include directory, and a conditional
  `add_subdirectory` — the only remaining feature check that changes the
  *shape* of the build rather than just a `#define`.
- On Windows it is always compiled: MinGW-w64 ships `regex.h` but puts the
  implementation in a separate `libregex`/`libgnurx`, so the probe correctly
  fails and the vendored copy is used. The old shell build worked around the
  same thing with a hardcoded `LDFLAGS="-lregex"` on MinGW.
- It is dead weight in the WASM binary later, for two extension checks.

## 3. Proposed change

**Filter in the caller, delete the backend.** No new match type: both call
sites already have a natural home for a three-line helper, and directory
listings here are tiny (`content/packages/`, `content/addons/<name>/`).

1. Add a small static helper to `src/options.c`:

   ```c
   // Case-insensitive check for one of a set of file extensions.
   static bool hasExtension (const char *name, const char *const *exts);
   ```

   and rewrite the two calls to list everything
   (`uio_getDirList (dir, "", "", match_MATCH_PREFIX)`, the idiom already used
   at `options.c:404`) and skip entries the helper rejects.

2. `src/libs/uio/match.h`: drop `#define HAVE_REGEX`, the `match_MATCH_REGEX`
   enum entry, `match_RegexContext`, the `#include <regex.h>`, and the four
   `match_*Regex` prototypes.

3. `src/libs/uio/match.c`: drop the six `#ifdef HAVE_REGEX` blocks
   (dispatch arms at lines 107, 137, 184, 213 and the implementation at
   487-568).

4. Delete `src/regex/` and its `CMakeLists.txt` / `Makeinfo`.

5. CMake: drop the `regcomp` check from `cmake/ConfigHeader.cmake`, the
   `uqm_regex` target and `src/regex` include directory from
   `CMakeLists.txt`, and the `if(NOT HAVE_REGEX)` branch from
   `sc2/src/CMakeLists.txt`. The converter's `HAND_WRITTEN` set loses
   `regex`.

6. Shell build (still alive until the c-making.md §8 parity gate): drop the
   `regex` branch from `sc2/src/Makeinfo`, `-Isrc/regex` from
   `sc2/Makeproject`, and `define_have_header regex.h` plus the MinGW
   `-lregex` from `sc2/build/unix/build.config`.

Behaviour is identical: `\.[rR][mM][pP]$` and a case-insensitive `.rmp`
suffix test accept exactly the same names.

## 4. Worth doing at the same time?

`match_MATCH_GLOB` is already dead — `HAVE_GLOB` is commented out at
`match.h:29`, and `match_matchGlob` (`match.c:430-480`) is a stub that has
never been reachable. Its only would-be caller is inside
`uio/debug.c:698`'s `uio_debugInteractive`, which is itself only called from
a commented-out line at `uqm/uqmdebug.c:142`. Removing the glob scaffolding
is the same edit in the same three files and takes uio's matcher down to the
four modes anyone uses. Flagged separately because it is not what the regex
removal requires.

## 5. Risk

Low. Two call sites, both with constant patterns, both reducible to a suffix
test; the replacement is exercised by the parity gate's "boots and loads
content" step, since `mountDirZips` is what mounts `content/packages/*.zip`
and the 3DO addons.
