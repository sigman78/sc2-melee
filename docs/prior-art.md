# Prior Art — existing ports and forks

Research snapshot: 2026-07-25. This informs the WASM and iOS port plans; see
`decisions.md` for what we adopted from each.

## WASM / browser

### intgr/uqm-wasm — the most relevant reference
<https://github.com/intgr/uqm-wasm>

- Forked **from this same SourceForge tree** (`sf.net/p/sc2/uqm`), so its diff
  applies almost directly to our codebase.
- Approach:
  - **Threading kept** — built with Emscripten **pthreads** (SharedArrayBuffer +
    Web Workers). SDL-thread backend did not work under Emscripten; the pthread
    backend did.
  - **Audio: OpenAL** backend (Emscripten ships an OpenAL-over-WebAudio
    implementation). MixSDL backend did not work.
  - **Persistence: IDBFS** (IndexedDB) for savegames and `uqm.cfg`, with a
    browser persistent-storage permission request.
  - Content served over HTTP; Docker + nginx dev setup; CI on GitHub Actions.
  - Built against Emscripten 3.1 (needed `libpng-mt` fix).
- Status: functional; last activity **Dec 2023**. Known issue: audio glitches in
  Firefox (WebAudio sample handling), Chrome OK.
- License: same as upstream (GPL-2.0 code), so we can cherry-pick freely.

### mikeakers/UQM-emscripten
<https://github.com/mikeakers/UQM-emscripten>

- Early work-in-progress (7 commits), effectively abandoned. Not useful beyond
  confirming others tried the ASYNCIFY-era approach before pthreads matured.

### davidben/uqm (Native Client)
<https://github.com/davidben/uqm>

- Historical NaCl port — dead platform, but demonstrates the codebase has been
  made to run in a sandboxed browser environment before.

## iOS / mobile

### njvack/uqm-ios
<https://github.com/njvack/uqm-ios>

- Old fork "with aims to port to iOS" (SDL 1.2 era, ~2011). Predates the SDL2
  backend; approach (Xcode project wrapping the tree + touch overlay) is still
  the right shape, but the code itself is outdated.

### Android port (libsdl-android, 2011, UQM 0.7.0)
- Proves the touch-control adaptation is workable: virtual d-pad / buttons for
  melee, tap-to-navigate for exploration. A reference for our touch design.

### SDL2 official iOS support
- SDL2 ships first-class iOS support (Metal-backed renderer, touch events,
  `SDL_UIKitRunApp` entry point). The Bumbershoot SDL2 porting write-up
  (<https://bumbershootsoft.wordpress.com/2019/12/16/porting-the-ur-quan-masters-to-sdl2/>)
  documents the SDL2 backend work that has since been merged into this tree.

## Actively maintained forks

### JHGuitarFreak/UQM-MegaMod
<https://github.com/JHGuitarFreak/UQM-MegaMod>

- Active fork: HD graphics, widescreen, QoL features, full SDL2 gamepad
  support with hotplugging, builds for Windows/macOS/Linux/Switch.
- Diverged heavily from vanilla; not a merge candidate for us, but a useful
  reference for controller support and for CMake-era build modernization ideas.

## Licensing note (matters for iOS)

- Code: **GPL-2.0**. Content: **CC BY-NC-SA 2.5**.
- GPL-2.0 has historically conflicted with App Store terms (VLC precedent);
  the NC clause on content also complicates any paid distribution.
- Realistic iOS distribution channels: personal sideloading, AltStore/dev
  builds, TestFlight (gray area), or EU alternative marketplaces. A public
  App Store release would need a relicensing effort by all copyright holders —
  out of scope. Recorded as a constraint in `decisions.md`.
