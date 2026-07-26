# CMake-ification plan (Phase 0)

Replace the custom shell/`Makeinfo`/`Makefile.build` system with modern
CMake, pruned of dead platforms from the first commit. Executes D1/D3; sets
up D2 (SDL3). Facts about the old system: `current-state.md` §Build system.

## Goals / non-goals

- **Goals:** one `CMakeLists.txt` tree that configures in seconds with zero
  interactive steps; works on Windows/Linux/macOS today and is the base for
  the `wasm` and `ios` presets later; dependencies resolved automatically
  (system first, fetch fallback); CI proving every commit.
- **Non-goals (now):** installers/packaging (CPack later), the old build's
  install/`.app`-bundling logic (revisit at release time), Symbian/WinCE/
  MSVC6 anything.

## 1. Pruned immediately (never ported into CMake)

| Old thing | Fate |
|---|---|
| `build/msvc6/` (MSVC6 projects), `src/config_vc6.h` | delete |
| `src/symbian/`, ARMV5/GCCE/WINSCW machinery, `INSTALL.symbian`, `uqm.lsm` era files | delete |
| WinCE (`cegcc`) cases | delete (they only live in shell scripts) |
| SDL1 build choices (`pure`/`opengl` graphics options) | not represented; CMake knows one SDL. SDL1 *code* dies in Phase 1 |
| `threadlib=pthread` option | not represented (threadlib itself dies in Phase 2) |
| `ovcodec=tremor` (fixed-point Vorbis, existed for Symbian) | not represented; standard libvorbis only |
| `mikmod=external` option | not represented; vendored mikmod always (it's current, 3.3.11.1) |
| x86 asm scalers (`2xscalers_{mmx,sse,3dnow}.c`, `scalemmx.h`) + `accel` menu | delete files; plain-C scalers only (SIMD intrinsics = backlog) |
| Vendored POSIX regex (`src/regex/`) and its `HAVE_REGEX` probe | delete — see `deregex.md` |
| `wspiapiwrap.{c,h}` (Windows 2000 `getaddrinfo` shim) | delete; ws2_32 has exported these since XP |
| Old shell build (`build.sh`, `build/unix/`, `Makefile.build`, `Makeproject`, `Makeinfo`s, `build.vars.in`, `src/config_unix.h.in`, `src/config_win.h.in`) | **kept until the parity gate (§8), then deleted** |

Netplay is *not* pruned — it stays as `UQM_NETPLAY` (default ON for
desktop; OFF in wasm/ios presets).

## 2. Layout

```
CMakeLists.txt              # project, options, deps, target uqm
cmake/
  Dependencies.cmake        # find-or-fetch logic
  ConfigHeader.cmake        # feature checks → config.h
  uqm.toolchain.wasm.cmake  # Phase 3
CMakePresets.json
sc2/src/**/CMakeLists.txt   # per-dir target_sources() (converted from Makeinfo)
```

- **One executable target `uqm`**; per-directory `CMakeLists.txt` call
  `target_sources(uqm PRIVATE …)` — preserves the old `Makeinfo`
  granularity and locality, keeps diffs reviewable.
- **Vendored code as separate targets** (compiled with warnings relaxed,
  linked into `uqm`): `uqm_mikmod` (`src/libs/mikmod`), `uqm_uio`
  (`src/libs/uio`, gets `HAVE_ZIP`); `src/getopt` is compiled conditionally
  on the feature checks (§4). No `uqm_md5`: nothing includes `md5.h`, and
  the old build never compiled it either.
- Language level: **C17** (`set(CMAKE_C_STANDARD 17)`), extensions on
  (code uses POSIX-isms on Unix). MSVC: `/utf-8`, permissive-minus is a
  stretch goal — first make it compile, record warnings as backlog.

## 3. Options (old menu → CMake)

| CMake option | Default | Replaces |
|---|---|---|
| `UQM_OPENAL` | OFF | `sound` choice (MixSDL always built; OpenAL additive, runtime `--sound=openal`) |
| `UQM_NETPLAY` | ON (desktop) | `netplay` full/none — keep the full/ipv4 distinction only if the code forces it; otherwise ON/OFF |
| `UQM_ZIP` | ON | `ioformat` stdio_zip |
| `UQM_CONTENTDIR` | `""` (= the checkout's `sc2/content`) | `--with-content` install path. The binary lives in `build/<preset>/`, so an empty value compiles in the in-tree content path rather than relying on `./content`; the runtime fallbacks are unchanged |
| `UQM_WERROR` | OFF | new; CI turns ON once the tree is warning-clean |

Everything else from the old menu (graphics, threadlib, accel, mikmod,
ovcodec, joystick) is a constant now — options that can only have one sane
value are not options.

## 4. Config header

Replace `config_unix.h.in`/`config_win.h.in` + `substitute_vars` with one
`config.h.in` + `configure_file()` driven by real checks:

- `check_symbol_exists(getopt_long "getopt.h" HAVE_GETOPT_LONG)` →
  else compile `src/getopt/`.
- `HAVE_ZIP` ← `UQM_ZIP`. `NETPLAY` ← `UQM_NETPLAY`. `HAVE_OPENAL` ←
  `UQM_OPENAL`.
- Endianness: `WORDS_BIGENDIAN` via CMake's built-in detection
  (`CMAKE_C_BYTE_ORDER`) — every live target is little-endian but the
  check costs one line and `endian_uqm.h` already consumes the macro.
- Paths: `CONTENTDIR`, `USERDIR` (`~/.uqm/` Unix, `%APPDATA%`-style on
  Windows — carry over the existing `config_win.h.in` values).
- Version: generate `uqmversion.h` content from `project(VERSION …)` +
  `git describe` (fallback string when git absent).
- Audit remaining `HAVE_*` uses (`grep -r "HAVE_" src/` at implementation
  time) — the old system also probed headers/types that modern compilers
  make moot; drop dead ones rather than porting their checks.

## 5. Dependencies — find-or-fetch

Pattern: CMake ≥ 3.24 `FetchContent_Declare(… FIND_PACKAGE_ARGS)` — uses
the system/vcpkg package when present, builds from a pinned source tarball
otherwise. Zero-setup on a fresh Windows machine, distro-friendly on Linux.

| Dep | find_package | Fetch fallback | Notes |
|---|---|---|---|
| **SDL** | `SDL3 CONFIG` → `SDL3::SDL3` | official SDL3 release tag | **Bootstrap exception — see §6** |
| zlib | `ZLIB` | `zlib-ng` (zlib-compat mode) or zlib tag | needed by uio-zip and libpng |
| libpng | `PNG` | libpng tag | used directly by `png2sdl.c` |
| ogg | `Ogg CONFIG` | ogg tag | official upstream ships CMake configs |
| vorbis | `Vorbis CONFIG` → `Vorbis::vorbisfile` | vorbis tag | ditto; stb_vorbis swap = backlog |
| OpenAL (opt) | `OpenAL CONFIG` (openal-soft) | openal-soft tag | only when `UQM_OPENAL=ON` |

Pin all fetch versions; `FETCHCONTENT_FULLY_DISCONNECTED` respected for
offline/distro builds. No git submodules.

## 6. SDL bootstrap (the one temporary wart)

The tree today compiles against SDL2; SDL3 migration is Phase 1. To keep
the parity gate (§8) meaningful — same code, new build system, comparable
binary — CMake bootstraps with an internal variable:

- `_UQM_SDL_BOOTSTRAP2` (undocumented, in `Dependencies.cmake` only):
  links SDL2 and defines `-DSDL_DIR=SDL2` exactly as the old build did.
- Everything else in the CMake tree is written SDL-version-agnostic (one
  `uqm::SDL` alias target, one include path variable).
- **Phase 1 deletes the variable and the SDL2 branch** — the alias flips to
  `SDL3::SDL3`; no other build-file change needed.

This keeps "build system replacement" and "SDL3 migration" bisectable as
independent changes.

## 7. Presets and CI

`CMakePresets.json`:
- `dev` — Ninja, Debug, host toolchain.
- `release` — Ninja, RelWithDebInfo.
- `wasm`, `ios` — placeholders now, filled in Phases 3/4.

The presets name no compiler, so the toolchain comes from the environment.
All three Windows toolchains build the tree as of the fixup pass: MSVC
19.44 (from a `vcvars64.bat` shell), MinGW-w64 GCC 15.2, and clang 21 with
`--target=x86_64-w64-mingw32 --sysroot=<mingw64>`. Keeping the MinGW/clang
path green is worth the effort beyond Windows: it is the closest local
proxy for the clang/lld toolchain Emscripten uses in Phase 3.

GitHub Actions (`.github/workflows/build.yml`): matrix over
ubuntu-latest / windows-latest / macos-latest × dev preset; steps =
configure, build, smoke test (`uqm --version` exits 0 — exercises
init/getopt without content), plus the Phase 2 burn-down grep once that
lands. Cache FetchContent + compiler cache (sccache/ccache).

> The smoke step must set `UQM_NO_MSGBOX=1`. On Windows and macOS the
> end-of-run log box is a modal dialog (`libs/log/msgbox_win.c`), and
> `--help` calls `log_showBox(true, …)`, so an unattended run blocks until
> someone clicks OK. `--version` never shows the box; `UQM_NO_MSGBOX`
> covers everything else, including crashes.

## 8. Execution order

1. **Converter script** (`tools/makeinfo2cmake.py`, throwaway): walk the
   ~100 `Makeinfo` files, emit per-dir `CMakeLists.txt`. The fragments are
   shell variable assignments plus a handful of conditionals (graphics
   module, getopt/darwin, netplay) — handle those four by hand.
2. Top-level `CMakeLists.txt` + `Dependencies.cmake` + config header; get a
   **link-complete build on this Windows machine** (dev preset), then
   Linux (WSL or CI) and macOS (CI).
3. **Parity gate:** CMake-built binary boots, plays the intro, loads a
   save, runs a melee round on all three OSes — behaviorally identical to
   a shell-build binary from the same commit.
4. CI workflow green on the matrix.
5. **Deletion commit:** everything in §1's "kept until parity" row +
   `INSTALL*` docs rewrite (a 20-line `INSTALL.md`: install CMake+Ninja,
   `cmake --preset dev && cmake --build --preset dev`).
6. Tag `pre-cmake` on the last commit where the shell build works (bisect
   anchor), matching the pre-flip tag convention from `unthread.md`.

### Status — Phase 0 complete

| Step | State |
|---|---|
| 1. Converter | done — 84 generated, 18 hand-written; deleted with the Makeinfos |
| 2. Link-complete build | done — Windows (MSVC 19.44, MinGW-w64 GCC 15.2, clang 21→mingw), Linux (GCC), macOS (clang) |
| 3. Parity gate | done — source lists matched at 346 files to the last commit before deletion; on Windows the binary boots, plays the intro to the interactive part, and runs a melee round. Loading a save was never exercised. |
| 4. CI | green on ubuntu / windows / macos |
| 5. Deletion commit | done |
| 6. `pre-cmake` tag | done |

Source-list parity was checked mechanically rather than by eye:
`tools/check-source-parity.sh` diffed CMake's own source list against
`build/unix/recurse` output from the same commit, and ran in CI on every
push. Both it and the converter died with the Makeinfos they compared
against; `pre-cmake` is the anchor if the comparison ever needs redoing.

The one gap worth remembering: the only behavioural testing was on Windows,
by hand. Linux and macOS are compile-and-`--version` only.

## 9. Risks

| Risk | Mitigation |
|---|---|
| Modern MSVC rejects 2003-era C (POSIX headers, K&R-isms) | clang-cl / MinGW escape hatch; fix forward — these fixes are wanted anyway for C17 hygiene |
| Makeinfo conditionals missed by the converter | only ~4 conditional fragments exist (graphics, getopt, darwin, netplay); convert those by hand, diff the final source list against `build_collect` output from the old system |
| Config-header symbol drift (old build defined something nobody noticed) | diff generated `config.h` against old `config_unix.h` on Linux before the deletion commit |
| FetchContent builds of png/vorbis misbehave on some host | pins + system-package precedence; CI covers all three OSes |
