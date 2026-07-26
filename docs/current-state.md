# Current State of the Codebase (port-relevant assessment)

Snapshot date: 2026-07-25, tree = SourceForge `sc2/uqm` main @ `d6583f225`.

This documents what exists today, as input to `options.md` and `plan-*.md`.

## Build system

- **Fully custom shell-based configure + GNU Make** — no autoconf, no CMake.
  Driver: `sc2/build.sh` → `sc2/build/unix/build.sh` + `build_functions` +
  `build.config` (interactive menu of choices: graphics, sound, ovcodec,
  mikmod, joystick, netplay, ioformat, accel, threadlib).
- Autoconf-like probes hand-rolled in `sc2/build/unix/config_functions`
  (`have_library`, `try_pkgconfig_lib`, endianness check, `@VAR@` template
  substitution into `config_unix.h` etc.).
- **Per-directory `Makeinfo` shell fragments** (~100 of them) declare sources
  (`uqm_CFILES`, `uqm_SUBDIRS`, …); `build/unix/recurse` walks them to emit a
  flat object list consumed by `sc2/Makefile.build`.
- Platform dispatch is a single `HOST_SYSTEM` variable (from `uname -s` or
  `BUILD_HOST` override) used in `case` statements everywhere. Documented
  cross-compile support exists (`build/unix/README.crossbuild`) and was used
  for MinGW, WinCE, and Symbian (ARMV5/GCCE/WINSCW with its own `bld.inf`
  native build under `sc2/src/symbian/`).
- Existing ports in-tree: Unix/Linux/BSD, Windows (MinGW + legacy MSVC6
  projects), macOS (Cocoa `SDLMain.m` shim + .app bundling in
  `build.config`), Symbian (dead), WinCE (dead).

Implication: the build system works but is a museum piece — interactive
shell menus, dead-platform special cases, and no IDE/toolchain integration.
Emscripten could technically be added as another `HOST_SYSTEM`, but both
target ports (WASM needs emcc + link flags + asset packaging; iOS needs an
Xcode project) fit far more naturally in CMake. See `decisions.md`.

## SDL status

- **SDL2 is the default graphics choice when available** (`build.config`
  `CHOICE_graphics_OPTIONS="pure opengl sdl2"`); SDL1.2 paths (`pure` = SDL1
  software, `opengl` = SDL1+GL) remain fully wired for legacy platforms.
- SDL1/SDL2 coexist in `sc2/src/libs/graphics/sdl/` gated by
  `#if SDL_MAJOR_VERSION` (`sdl1_common.c`, `sdl2_common.c`, `sdl2_pure.c`,
  `pure.c`, `opengl.c` — the last is SDL1-only; there is no SDL2+OpenGL path,
  SDL2 uses `SDL_Renderer`/`SDL_UpdateTexture`).
- Version-agnostic SDL use in input (`libs/input/sdl/`), threads
  (`libs/threads/sdl/sdlthreads.c`), timers (`libs/time/sdl/`), and the
  MixSDL audio driver (`libs/sound/mixer/sdl/audiodrv_sdl.c`).
- Include-path selection via `-DSDL_DIR=SDL|SDL2` and the `SDL_INCLUDE()`
  macro in `sc2/src/port.h`.

## Dependencies

| Library | Role | Vendored / system |
|---|---|---|
| SDL 1.2 or SDL2 | video/input/timers/threads/audio backend | system (one required) |
| libpng | PNG loading | system (required) |
| zlib | .zip resource archives (`ioformat=stdio_zip`, default) | system |
| libogg/libvorbis (or Tremor) | music/speech decoding | system |
| libmikmod | tracker music (.mod) | **vendored** `src/libs/mikmod/` (default) or system |
| OpenAL | alternate sound backend ("experimental") | system, optional |
| OpenGL | SDL1 accel path only | system, optional |
| pthread | alternate thread backend (`threadlib=pthread`) | system, optional |
| getopt_long, regex | CLI parsing, patterns | **vendored fallbacks** `src/getopt/`, `src/regex/` |
| uio | virtual filesystem (dir/zip mounts) | **vendored** `src/libs/uio/` |
| md5 | checksums | **vendored** `src/libs/md5/` |
| platform sockets | Supermelee netplay (`netplay=full|ipv4|none`) | system |

## Threading and main loop

### Thread architecture
The game is **structurally multi-threaded** — this is the hardest part of the
WASM port:

| Thread | Created at | Role |
|---|---|---|
| main thread | `main()` (`src/uqm.c:447` loop) | event pump (`TFB_ProcessEvents`), thread lifecycle processing, and **all rendering** (`TFB_FlushGraphics` drains the draw-command queue; "only call from main() thread") |
| `Starcon2Main` | `src/uqm.c:445` → `src/uqm/starcon.c:154` | **the entire game logic** — state machine, battle, planet exploration, comm; paces itself with `SleepThreadUntil` (battle ≈24 Hz, `BATTLE_FRAME_RATE`) |
| audio stream decoder | `libs/sound/stream.c:792` | decodes music/speech into ring buffers |
| (internal) SDL/OpenAL audio callback thread | driver init | real-time mixing |

- Cross-thread communication: draw-command queue (`libs/graphics/dcqueue.c`)
  with a recursive mutex + condvar (producer blocks when full), shared
  globals, per-source mutexes.
- Thread backend chosen at build time: `THREADLIB_SDL` (default) or
  `THREADLIB_PTHREAD` (`libs/threads/{sdl,pthread}/`). Primitives: Mutex,
  Semaphore, RecursiveMutex, CondVar (`libs/threadlib.h`).
- Quirk: `CreateThread` is deferred — spawn requests queue up and the real OS
  thread is created only when the main thread calls
  `ProcessThreadLifecycles()` each loop iteration.

### Blocking constructs (must be handled for WASM)
`SleepThread(Until)`/`TaskSwitch`/`HibernateThread` → `SDL_Delay`/`usleep`;
`WaitCondVar` when the draw queue fills; `ConcludeTask`'s busy-wait;
blocking `WaitThread` joins. Timer base is `ONE_SECOND = 840` ticks over
`SDL_GetTicks()` (`libs/timelib.h`, `libs/time/sdl/`).

### No single-threaded mode exists
Only `THREADLIB_SDL`/`THREADLIB_PTHREAD`. However, upstream comments say
threading is a wart they intend to remove (`uqm.c:370` "TODO: Once threading
is gone…"), and the threadlib abstraction is clean enough that a
`THREADLIB_NONE` cooperative backend is a natural insertion point. No
`__EMSCRIPTEN__` references exist anywhere in the tree.

## Audio and video

### Audio
- Three backends (`src/libs/sound/audiocore.h`): **MixSDL** (default —
  custom software mixer fed by the SDL audio callback,
  `libs/sound/mixer/sdl/audiodrv_sdl.c`), **OpenAL**
  (`libs/sound/openal/audiodrv_openal.c`, behind `HAVE_OPENAL`), and
  **nosound**. Runtime-selectable via `--sound=`, falls back gracefully.
- **Audio decoding runs on its own thread**: `InitStreamDecoder()` spawns an
  "audio stream decoder" task (`libs/sound/stream.c:786`) that decodes/queues
  buffers under a per-source mutex. This is the main threading hotspot for a
  WASM port.
- Decoders registered in a static table (`libs/sound/decoders/decoder.c:133`):
  wav, mod (vendored mikmod), ogg (libvorbis or Tremor), duk (video audio
  track), aif. Vendored mikmod compiles **only** the no-op output driver;
  UQM registers its own memory-buffer driver (`modaud.c`) — mikmod never
  touches OS audio APIs. No ALSA/OSS/esd/dev-node access anywhere.

### Video
- Intro/ending movies are the 3DO-era **DUCK codec** (`.duk`), fully
  software-decoded in `libs/video/dukvid.c` into RGB canvases. **No SDL YUV
  overlays anywhere** — a common SDL2/WASM breakage source that simply
  doesn't exist here. Frame advance is audio-synced and driven from the main
  render loop (`libs/video/vidplayer.c`); the audio track uses the normal
  decoder thread.

### Graphics pipeline
- Software rendering at a fixed logical 320×240 (2× offscreen for scalers)
  presented via **SDL2 `SDL_Renderer` + streaming `SDL_Texture`**
  (`libs/graphics/sdl/sdl2_pure.c`: `SDL_UpdateTexture` → `SDL_RenderCopy` →
  `SDL_RenderPresent`, with `SDL_RenderSetLogicalSize` for window scaling).
  This maps cleanly to Emscripten SDL2 (WebGL) and iOS SDL2 (Metal).
- The SDL1 fixed-function OpenGL path (`opengl.c`) is **dead code under
  SDL2** (`sdl2_common.c:92` always routes to the pure path).
- Software scalers (bilinear/biadapt/biadv/triscan/hq2x) have x86
  MMX/SSE/3DNow variants that must fall back to portable C on WASM/ARM —
  already a build choice (`accel` menu item).
- Only legacy/irrelevant OS-specific code found: CD-audio playback under
  `libs/cdp/` (Windows-specific, unrelated to core pipeline).

## File I/O, content, networking

### uio virtual filesystem (`src/libs/uio/`)
- Custom VFS: POSIX-like ops over a merged tree of mounted backends —
  **stdio** (always) and **zip** (behind `HAVE_ZIP`, zlib). Mounts stack
  (TOP/BOTTOM/ABOVE/BELOW, optional read-only), so base content, addons, and
  zip packages layer into one logical tree.
- **Content can be read directly from `.zip`/`.uqm` archives** (a `.uqm` is
  just a zip): `mountDirZips()` in `src/options.c` mounts every matching
  archive in `content/packages/` and `content/addons/<name>/` without
  extraction. This is ideal for web delivery (fetch one archive → mount).
- Content dir resolution: `--contentdir`, compiled-in `CONTENTDIR`,
  `./content`, macOS bundle-relative probe; first dir with a `version` file
  wins.

### Content
- `sc2/content` in this repo holds real assets, **~172 MB total**: base
  39 MB, 3DO voice pack 115 MB, 3DO music 19 MB. The 3DO **video** addon is
  only a 5 KB manifest — its payload (~200+ MB) is distributed separately.
- Implication: far over web/mobile bundle budgets — needs staged/on-demand
  delivery for WASM (base first, voice/music/video optional fetch) and
  On-Demand Resources or in-app download for iOS.

### Saves and config
- All persistence goes through uio→stdio: config dir defaults to `~/.uqm/`
  (`UQM_CONFIG_DIR`/`--configdir` override), saves under `save/`, melee teams
  under `teams/`. Save files are tens of KB — trivially IDBFS-compatible.
- One WASM gotcha: `getHomeDir()` (`src/libs/file/dirs.c`) falls back to
  `getpwuid()` — must force the config dir via env/flag under Emscripten.

### Networking (Supermelee netplay)
- BSD sockets (+ parallel Winsock impl), `select()`-based, in
  `src/libs/network/` + `src/uqm/supermelee/netplay/`.
- **Cleanly optional at configure time**: `netplay=none` leaves `NETPLAY`
  undefined and all call sites are `#ifdef NETPLAY`-guarded — an exercised,
  supported configuration. WASM/iOS ports can simply disable it initially;
  netplay-over-WebSocket would be a separate later project.

### Misc
- `getopt_long` with vendored fallback; env vars `UQM_CONFIG_DIR`,
  `UQM_SAVE_DIR`, `UQM_MELEE_DIR`, `HOME`, `TMP`/`TEMP`.
- `src/symbian/config.h` + macOS bundle probing in `options.c` are the
  in-tree precedents for "restricted platform, content relative to bundle" —
  the template for both iOS bundle paths and the Emscripten MEMFS mount.
- No `src/ios/`, `src/android/`, or `src/emscripten/` exists yet — new work,
  following the Symbian/darwin precedent.
