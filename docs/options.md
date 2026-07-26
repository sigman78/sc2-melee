# Options Analysis

Inputs: `current-state.md` (codebase facts), `prior-art.md` (existing ports).
Chosen outcomes are recorded in `decisions.md`. Guiding directive: **modernize
the project and the porting foundation** — prefer current tooling/libraries
over minimally-invasive patching — while shipping WASM first, iOS second.

---

## 1. Build system

| Option | Pros | Cons |
|---|---|---|
| **A. Keep custom shell build**, add Emscripten as another `HOST_SYSTEM` | Zero migration cost; cross-build machinery exists | Museum piece; interactive menus; no IDE/Xcode integration (iOS effectively impossible without hand-maintained Xcode project); nobody new can contribute |
| **B. CMake as the new canonical build** | Industry standard; first-class Emscripten support (`emcmake`); generates Xcode projects for iOS (incl. code signing, asset embedding); presets; FetchContent for deps; CI-friendly | One-time effort to translate ~100 `Makeinfo` fragments (mechanical — they're just file lists); need to replicate config-header generation |
| C. Meson/other | Also modern | No advantage over CMake for this project; CMake's Xcode generator is the iOS decider |

**Assessment:** B. The `Makeinfo` files are plain file lists with occasional
conditionals — a script can convert them. The old build can coexist during
transition and be removed once CI proves parity. Both target platforms
(Emscripten, iOS) are exactly where CMake is strongest.

## 2. SDL version: SDL2 vs SDL3

Facts: the tree already defaults to SDL2 (`SDL_Renderer` streaming-texture
path, clean); SDL1.2 paths remain for dead platforms. SDL3 (stable since
2025) is where all current development happens: official Emscripten support,
Metal-backed iOS support, and the `SDL_AppIterate` main-callbacks model that
natively fits browser and iOS lifecycles. The proven intgr/uqm-wasm diff is
SDL2-based. Emscripten's SDL2 port remains maintained.

| Option | Pros | Cons |
|---|---|---|
| A. Stay on SDL2 | Proven WASM diff applies nearly verbatim; zero API migration | Not "current"; SDL2 is in maintenance mode; misses `SDL_AppIterate`, which is the cleanest answer to both target platforms' main-loop inversion |
| **B. Migrate to SDL3, drop SDL1.2 entirely** | Modernization directive satisfied; one API for desktop+WASM+iOS; `SDL_AppIterate` callbacks solve the Emscripten main-loop problem *and* iOS lifecycle in one mechanism; best touch/gamepad/hidpi support | Migration effort across the ~6 SDL-touching modules (graphics, input, threads, time, audio driver, event pump); loses direct reuse of the intgr diff (its *approach* still transfers) |
| C. SDL2 API on sdl2-compat (SDL3 runtime) | Cheap "runs on SDL3" | Doesn't modernize the code; compat layer on Emscripten/iOS is the least-tested configuration |

**Assessment:** B. The SDL surface in UQM is deliberately narrow (a handful
of files under `libs/*/sdl/`), the SDL2→SDL3 migration guide is mechanical
for this API subset, and SDL3's callback model is *the* modern answer to the
two platforms we're targeting. Drop SDL1.2 (`pure.c` SDL1 paths, `opengl.c`,
Symbian) in the same stroke — it halves the `#if SDL_MAJOR_VERSION`
conditionals.

## 3. Threading model for WASM

Facts: game logic lives on its own thread; no single-thread mode exists;
upstream wants threads gone eventually. Two viable strategies:

| Option | Pros | Cons |
|---|---|---|
| A. **Emscripten pthreads** (SharedArrayBuffer + Workers), keep architecture | Proven — intgr/uqm-wasm shipped this way; smallest code delta; blocking sleeps/condvars just work | Requires cross-origin isolation (COOP/COEP headers) on the host — impossible on plain GitHub Pages without a `coi-serviceworker` shim; per-thread memory overhead; audio-worklet interactions trickier |
| B. **Single-threaded refactor** (`THREADLIB_NONE` cooperative backend) + ASYNCIFY/**JSPI** for the blocking sleeps in game logic | Aligns with upstream's own TODO; no COOP/COEP requirement (deploy anywhere); simpler debugging; JSPI (shipping in Chrome/Firefox since ~2025) has near-zero overhead vs old ASYNCIFY bloat | Real refactor risk: `Starcon2Main`'s deep call stacks block on `SleepThreadUntil` everywhere; the audio decoder must be re-driven from the main loop; largest unknown in the whole project |
| C. Hybrid: pthreads first, converge to single-thread later | De-risks shipping; modernization lands as phase 2 | Two migrations touch the same code |

**Assessment:** For "WASM working first," A is the proven route and JSPI
maturity makes B attractive as the *end state*. Recommended: **C staged as
A-then-B** — ship pthreads WASM (with `coi-serviceworker` for static hosts),
then implement `THREADLIB_NONE` as the modernization end-game (it also
benefits iOS battery/perf marginally and upstream wants it anyway). Decision
may be revisited if early SDL3+pthreads+Emscripten testing hits friction.

## 4. Audio backend for WASM

| Option | Notes |
|---|---|
| **A. OpenAL** | Proven under Emscripten (maps to Web Audio; intgr used it). Already implemented in-tree behind `HAVE_OPENAL`. On iOS: OpenAL framework is deprecated by Apple but functional; alternatively openal-soft compiled in |
| B. MixSDL (SDL audio callback) | Default elsewhere; historically broken under Emscripten (2023); SDL3's audio-stream model may have fixed this — worth a cheap experiment since it would unify all platforms on one backend |

**Assessment:** Try MixSDL-on-SDL3 first (one experiment, big
simplification if it works — SDL3 audio on Emscripten uses AudioWorklets);
fall back to OpenAL which is known-good. Keep both compiled where possible.

## 5. Content delivery (WASM)

Facts: ~39 MB base + 19 MB music + 115 MB voice + (~200 MB video, not in
repo); uio mounts `.uqm`/`.zip` archives directly without extraction.

| Option | Notes |
|---|---|
| A. `--preload-file` MEMFS bundle | Simple but forces full download before boot; 39 MB+ up front; no caching granularity |
| **B. Fetch `.uqm` zip packages over HTTP at boot, cache, mount via uio zip backend** | Matches the existing packaging format exactly; base pack first (playable), music/voice/video fetched optionally (settings toggle or background fetch); browser-cacheable |
| C. WasmFS + OPFS lazy per-file | Most "modern" but uio already provides the archive abstraction; per-file HTTP for thousands of small files is worse than zip mounts |

**Assessment:** B. The game's own addon architecture (base required, addons
optional, zip-mounted) is *already* a progressive-download design. Store
fetched packs in OPFS/IDBFS so repeat visits are offline-capable. Saves and
config on IDBFS with `FS.syncfs` after writes (proven by intgr), config dir
forced via `UQM_CONFIG_DIR` (avoids the `getpwuid` fallback).

## 6. Netplay

Compiles out cleanly (`netplay=none`, exercised config). **Disable for WASM
and iOS initially.** A WebSocket/WebRTC transport is a self-contained future
project (the socket layer is already abstracted behind `libs/network/`).

## 7. iOS approach

- **Toolchain:** CMake → Xcode generator, SDL3's iOS backend (Metal),
  `SDL_AppIterate` callbacks satisfy the UIKit lifecycle (no main-loop
  ownership fight). A thin `src/ios/` platform dir following the
  darwin/symbian precedent (bundle-relative content dir, Documents for
  config/saves).
- **Input:** the game is keyboard/joystick driven via an existing virtual
  controller layer (`libs/input/sdl/vcontrol.c`) — map to (a) touch overlay
  (virtual d-pad/buttons, Android-port precedent), (b) SDL3 game
  controllers (MFi/PS/Xbox pads work out of the box on iOS), (c) iPad
  hardware keyboards. Melee needs the overlay; exploration/menus can also
  use tap-based navigation later.
- **Content:** bundle base+music (~58 MB — fine for an app), voice/video via
  in-app download or On-Demand Resources.
- **Distribution constraint:** GPL-2.0 code + CC BY-NC-SA content — public
  App Store release is legally fraught (see `prior-art.md`). Target:
  personal/dev builds, AltStore/sideloading, EU alt-marketplaces.

## 8. Modernization scope (beyond the ports)

Worth doing as the foundation (roughly in order of leverage):
1. **CMake build + GitHub Actions CI** (Linux/Windows/macOS/Emscripten
   matrix) — everything else hangs off this.
2. **Delete dead platforms**: Symbian, WinCE, MSVC6 projects, SDL1.2 paths.
3. **SDL3 migration** (drop SDL1/SDL2), including `SDL_AppIterate`
   restructure of `main()`'s pump loop.
4. **Dependency hygiene**: system/FetchContent libpng, zlib, ogg/vorbis
   (or stb_vorbis to shed two deps); evaluate upgrading/replacing vendored
   mikmod (it's already 3.3.11.1, recently upgraded — low priority).
5. **C standard**: the code is C89-flavored; move to C11/C17 baseline
   (fixed-width types over `DWORD`-style typedefs, `stdbool`, remove
   `port.h` shims that compilers made obsolete). Mechanical but large;
   do opportunistically, not as a blocking phase.
6. x86-asm scalers → keep C fallbacks only (or SIMD-everywhere via
   intrinsics later; WASM SIMD is a stretch goal).
