# Decisions

Lightweight ADR log. Status values: **proposed** (recommended by analysis,
awaiting owner sign-off), **accepted**, **superseded**. Rationale details in
`options.md`.

---

## D1. CMake becomes the canonical build system — *accepted (2026-07-25)*
Replace the custom shell/`Makeinfo` build with CMake (+ presets). Old build
kept temporarily until CI proves parity, then deleted. Drivers: Emscripten
(`emcmake`) and iOS (Xcode generator) support; contributor accessibility.

## D2. Target SDL3; drop SDL 1.2 support entirely — *accepted (2026-07-25)*
Migrate the SDL layer (graphics/input/time/threads/audio-driver/event pump)
from SDL2 to SDL3 and delete SDL1-only paths (`sdl1_common.c`, SDL1 branches
in `pure.c`/`sdl_common.c`, `opengl.c` fixed-function renderer). Adopt
`SDL_MAIN_USE_CALLBACKS` / `SDL_AppIterate` as the main-loop structure — this
is the load-bearing choice that makes both Emscripten and iOS lifecycles
native rather than fought-against.
Consequence: the intgr/uqm-wasm SDL2 diff is used as a map, not a patch.

## D3. Dead platforms removed — *accepted (2026-07-25)*
Symbian (`src/symbian/`, ARMV5/GCCE/WINSCW machinery), WinCE, MSVC6 project
files (`build/msvc6/`) are deleted. macOS/Windows/Linux stay (now via CMake).

## D4. Un-thread BEFORE the WASM port, by direct flattening (no coroutine bridge) — *accepted (2026-07-25; twice revised same day)*
User decisions: (a) threading must be gone to enable a proper WASM port,
with de-threading done on desktop first; (b) the interim coroutine bridge
is dropped — go straight to the DOS-style flat game loop. Enabler: every
flattening conversion (DoInput screens → state stack, inline sequences →
mini state machines) lands incrementally under the still-threaded build,
which stays playable throughout and serves as the A/B oracle; the thread is
removed last, gated by a CI burn-down of blocking call sites. Result: the
WASM build needs no pthreads, no SharedArrayBuffer, no Asyncify — a plain
`SDL_AppIterate` loop. Push-model MixSDL conversion included. Full map:
`unthread.md`. Coroutines remain only as a recorded fallback for an
intractable subsystem (`unthread.md` §9).

## D5. WASM audio: MixSDL-on-SDL3 experiment, OpenAL fallback — *proposed*
One time-boxed experiment with the default MixSDL driver under SDL3/
Emscripten (AudioWorklet path). If artifacts appear, switch to the in-tree
OpenAL backend (proven under Emscripten via emscripten's OpenAL-over-WebAudio).

## D6. WASM content: HTTP-fetched `.uqm` zip packs mounted via uio — *proposed*
Base pack (~39 MB) fetched at boot with a progress screen; music/voice packs
optional (user-triggered or background fetch); packs cached in OPFS/IDBFS for
offline replay. Saves/config on IDBFS with `FS.syncfs`; config dir forced via
`UQM_CONFIG_DIR=/home/web_user/.uqm` equivalent (avoids `getpwuid`).

## D7. Netplay disabled on WASM and iOS — *proposed*
Build with `NETPLAY` undefined (already a supported configuration).
WebSocket/WebRTC transport is a possible future project, out of scope.

## D8. iOS: SDL3 + CMake-generated Xcode project, sideload distribution — *accepted (2026-07-25)*
Addendum: voice pack ships **in** the iOS install (user preference); details
deferred until Phase 4.
New `src/ios/` platform dir (bundle-relative content, Documents for
config/saves), touch overlay mapped through the existing `vcontrol` layer,
MFi/PS/Xbox controllers and hardware keyboards supported natively via SDL3.
Base+music+voice content bundled in the install (~173 MB).
**Constraint (accepted as fact):** GPL-2.0 + CC BY-NC-SA makes a public App
Store release legally fraught — target dev/sideload/AltStore/EU-alt-store
distribution; do not plan App Store submission.

## D9. Modernization is the foundation phase, not an afterthought — *accepted (user directive, 2026-07-25)*
Current tooling and libraries for both the project and the porting
foundation: CMake, CI (GitHub Actions), SDL3, dead-code removal, dependency
hygiene. C11/C17 cleanups land opportunistically, never as a blocking phase.

---

### Open questions — resolved 2026-07-25
1. SDL3-now: **accepted** (D2).
2. Deleting dead platforms / diverging from upstream: **accepted** (D3).
3. Hosting: **Cloudflare or a dedicated server** — header control available,
   but D4 makes the build header-independent anyway.
4. iOS voice pack: **bundled in the install**; revisit specifics in Phase 4.

## D9. Green-field rewrite of the GAME logic, not only the engine — *accepted (2026-07-26)*
Owner decision, reversing the recommendation in `rewrite-plan.md`. That plan
proposed porting `uqm/ships/` and `uqm/comm/` verbatim and rewriting only
`libs/`. It optimised for preservation; the owner's goal is a codebase worth
working in, in which the game logic is the part that gets rebuilt clean.
The existing C is read as an executable specification and discarded.
Plan: `game-rewrite-plan.md`. The engine-side findings and deletion inventory
in `rewrite-plan.md` remain valid; its game-side recommendations are withdrawn.

## D10. "Same game" means same design and feel, not frame-exact — *accepted (2026-07-26)*
Consequence of D9, and it has to be stated because the two pull in opposite
directions. A reimplementation cannot be frame-exact, so CRC/frame-hash
oracles are rejected as a gate. Verification is instead: dialogue transcript
fixtures, content-alignment checks in CI, melee round-robin tournaments
compared on win rate and match-length distribution, and property tests per
component. Behaviour will differ in detail and be tuned back by play; a
per-ship tuning pass is budgeted as the largest irreducibly-human block.
Corollary, decided now: lockstep netplay is out of scope for the first
release, because it would constrain the loop model and must be settled
before M1 fixes it.

## D11. 3DO support dropped — *accepted (2026-07-26)*
Owner decision. Removes `libs/video` (the DUCK FMV player, 2,365 lines) and
the `3dovoice` addon path. Note this is a feature removal, not dead-code
cleanup: `libs/video` is reachable from `intro.c:866-901`, and the voice pack
replaces six races' dialogue *text* outright, so it is not an audio-only cut.
