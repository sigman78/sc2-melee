# Roadmap: Modernize → Un-thread → WASM → iOS


> **Retargeted (2026-07-26).** This roadmap describes modernising UQM in
> place. That work happened and is on `modernize/wasm`: CMake, SDL3, the
> vendored-dependency purge, and the un-threading sweep (blocking sites
> 106 → 44). The browser build runs.
>
> The project has since forked to `rewrite/core` for a green-field rewrite
> (D9, D10). Phases 0–3 below are history; Phase 4+ is superseded by
> `game-rewrite-plan.md`. This file is kept for the sequencing rationale and
> because `modernize/wasm` is still built and kept in CI — it is the only
> reference the "does it feel right" questions have.

Phases are sequential; steps within a phase are ordered but often
parallelizable. Each phase ends with a working game on all previously
working platforms (CI-enforced once Phase 0 lands). References: facts in
`current-state.md`, choices in `decisions.md`.

---

## Phase 0 — Build foundation (CMake + CI)  [D1, D3 — execution plan in `c-making.md`]

0.1 Write `CMakeLists.txt` skeleton: one target `uqm`, options mirroring the
    old menu (`UQM_SOUND=mixsdl|openal`, `UQM_NETPLAY=OFF|…`, `UQM_ZIP=ON`,
    `UQM_ACCEL=…`), config header generated via `configure_file` replacing
    `config_unix.h.in` substitution.
0.2 Convert `Makeinfo` file lists → CMake sources (scripted conversion; the
    fragments are shell-set file lists with a few conditionals).
0.3 Dependencies via `find_package` + FetchContent fallback: SDL, libpng,
    zlib, ogg/vorbis. Vendored mikmod/uio/md5/getopt/regex stay in-tree as
    object libraries.
0.4 Prove parity: build + boot on Windows (this machine), Linux, macOS.
0.5 GitHub Actions CI matrix (Linux, Windows, macOS) doing configure+build+
    smoke run (`--help` / headless init where possible).
0.6 Delete dead platforms: `src/symbian/`, WinCE cases, `build/msvc6/`
    (keep the old shell build until Phase 1 completes, then remove).

Exit criteria: `cmake --preset default && cmake --build` produces a playable
game on the three desktop OSes; CI green.

## Phase 1 — SDL3 migration  [D2]

1.1 Delete SDL1.2 code paths (`sdl1_common.c`, `#if SDL_MAJOR_VERSION == 1`
    branches, `opengl.c`, `pure.c` SDL1 half, `darwin/SDLMain.m`).
1.2 Mechanical SDL2→SDL3 API migration in `libs/graphics/sdl/`,
    `libs/input/sdl/`, `libs/time/sdl/`, `libs/threads/sdl/`,
    `libs/sound/mixer/sdl/` (rename pass per SDL3 migration guide; audio
    driver moves to `SDL_AudioStream`).
1.3 Restructure `main()` to `SDL_MAIN_USE_CALLBACKS`: `SDL_AppInit` = current
    init, `SDL_AppIterate` = one iteration of the `uqm.c:447` pump loop
    (events → `ProcessThreadLifecycles` → `TFB_FlushGraphics`),
    `SDL_AppEvent` = event handling, `SDL_AppQuit` = uninit. Game logic stays
    on the `Starcon2Main` thread for now.
1.4 Desktop QA pass: fullscreen/scalers/gamepads/video playback/save-load.

Exit criteria: SDL3-only tree, desktop CI green, no regressions in a
playthrough smoke test (start game, melee battle, planet landing, comm,
save/load, intro video).

> **Outstanding:** Linux and macOS are verified to compile and run
> `--version` in CI only -- the game has never been *run* on either. Steps
> 1.1/1.2 shipped on that basis because the SDL3 work was Windows-tested by
> hand. Owner decision (2026-07-25): do the Linux/macOS playthrough pass
> once the browser build is working acceptably, rather than blocking Phase 2
> on it. Track it as a release gate, not a phase gate.

> **Checked, not a defect:** the Setup menu's graphics options looked
> inert during play-testing. They are not -- resolution and fullscreen
> changes are applied when the settings menu is exited, not live, which is
> how `setupmenu.c` has always worked. The one genuinely hidden option is
> the graphics-driver choice, gated on `HAVE_OPENGL`; that was equally
> hidden under SDL2, since the old build never defined it either.

## Phase 2 — Un-threading by direct flattening  [D4 — full map in `unthread.md`]

Ordered before the WASM port so Emscripten never sees threads. Every step
lands on a playable desktop build; the game stays threaded until the final
flip.

2.1 MixSDL → push model (`SDL_AudioStream` fed from the main loop) +
    stream-decoder pump + nosound pump. Independent PRs, work under the
    threaded build.
2.2 State-stack driver; convert the `DoInput` trampoline and 2–3 pilot
    screens to prove the pattern (incl. `CHECK_ABORT` unwind semantics).
2.3 Sweep remaining `DoInput` screens (~48 sites; mechanical, batched PRs).
2.4 Sweep inline blocking sequences (~40–60: comm animations, FMV, lander,
    credits, fades) — the QA-heavy part; per-subsystem playthrough checks.
2.5 Burn-down gate: CI grep for blocking primitives outside the driver
    reaches zero (`unthread.md` §7.5).
2.6 The flip: state driver runs from the main loop; delete `Starcon2Main`
    spawn and threadlib backends; tag the last threaded build for bisects.

Exit criteria: single OS thread (plus SDL-internal audio drain), full
playthrough checklist passes on all three desktop OSes, burn-down at zero.

### Status (2026-07-25)

2.1 done. 2.2 done: state-stack driver, `libs/coroutine.h` as the conversion
notation (owner decision — see `unthread.md` §3a), and `restart.c` converted
end to end as the pilot. 2.5 done: `tools/unthread-burndown.sh` runs in CI
with a budget that only moves down; it stands at **86** of the original 106.

2.4 first pass done for the cutscenes: `fmv.c`, `credits.c` and `intro.c` are
flattened, so no cutscene blocks any more.

That pass also turned up, and fixed, a general defect that would have
poisoned the whole sweep: the main menu had been deaf to keyboard input since
the pilot landed, because a flattened screen that paces itself with
`UQM_SLEEP` runs the input preamble several times per screen frame and
`PulsedInputState` is edge-triggered, so the keypress was consumed before the
screen read it. The conversion notation now distinguishes pacing yields from
frame yields (`UQM_PACING`), restoring `DoInput`'s one-sample-per-frame
invariant. Detail in `unthread.md` §7a.

2.3 (~34 `DoInput` screens) and the rest of 2.4 (comm, lander, melee) are the
remaining grind. `unthread.md` §7a also carries the open question about where
pause/sleep/exit-confirm live once the main loop owns the frame, and a known
undercount in the 2.5 gate.

## Phase 3 — WASM port (single-threaded)  [D4, D5, D6, D7]

3.1 Emscripten toolchain file + `wasm` CMake preset (`emcmake`); netplay
    off, accel off (C scalers); SDL3 via Emscripten port/FetchContent.
    Plain main loop — no pthreads, no Asyncify, no special link flags
    beyond FS/loader setup (the Phase 2 payoff).
3.2 Filesystem: MEMFS + IDBFS mount for config/saves (`UQM_CONFIG_DIR`
    forced; `FS.syncfs` after save-file writes); content packs fetched as
    `.uqm` zips into MEMFS/OPFS and handed to the existing uio zip mounts.
3.3 Content pipeline: script to pack `content/base` → `base.uqm`,
    music/voice → addon `.uqm`s (format already exists); loader HTML/JS
    with fetch progress + pack selection; cache packs in OPFS.
3.4 Audio: push-model MixSDL on SDL3-Emscripten (AudioWorklet); OpenAL
    fallback per D5. User-gesture audio unlock in the loader page.
3.5 Video: `.duk` software decode should Just Work (pure C); verify perf.
3.6 Hosting & deploy: Cloudflare (Pages/Workers or dedicated server) — no
    special headers needed since the build is single-threaded; CI job
    building and deploying on tag.
3.7 QA: Chrome/Firefox/Safari matrix (Safari much lower-risk without SAB).

Exit criteria: full game playable in browser from a URL, saves persist
across reloads, base pack < ~40 MB initial download, smooth 60 fps pump /
24 Hz battle on a mid-range laptop, plain static hosting.

### Status (2026-07-25)

3.1 done: `cmake --preset wasm && cmake --build --preset wasm` produces
uqm.html/js/wasm/data with Emscripten 6.0.4. **The game boots in Chrome**,
renders through the `opengles2` (WebGL) renderer, takes keyboard input,
navigates menus, and reaches Super Melee. Audio opens at 44100 Hz stereo
through Emscripten's backend. Artifacts total ~16 MB (13 MB content,
2 MB wasm, 1 MB js), inside the 40 MB budget.

3.2 done: saves/config persist to IndexedDB via an IDBFS mount at `/uqm`.

**Still threaded.** `Starcon2Main` exists, so the build needs `-pthread`,
which needs SharedArrayBuffer, which needs COOP/COEP headers from the host
(`tools/serve-wasm.py` sends them). SDL3 itself defaults `SDL_PTHREADS` off
for Emscripten for this reason and has to be overridden. Finishing
`unthread.md` §7.6 is what removes all of that and makes plain static
hosting work -- which is what 3.1 means by "no special link flags".

Not done: 3.3 (content packs / on-demand addon fetch -- only the base pack
is preloaded, so there is no music or speech), 3.4 audio QA, 3.5 video,
3.6 hosting, 3.7 browser matrix.

## Phase 4 — iOS (iPad) port  [D8]

4.1 `ios` CMake preset → Xcode project; SDL3 iOS backend; `src/ios/`
    platform dir (bundle-relative `CONTENTDIR`, config/saves in
    `Documents/`, following the darwin/symbian precedent in `options.c`).
4.2 Content: bundle `base.uqm` + music + voice packs in the install (~173 MB
    app — accepted; see D8 addendum). Details deferred to this phase.
4.3 Input: touch overlay (virtual d-pad + 2 buttons for melee; menu
    navigation via taps) mapped through `libs/input/sdl/vcontrol.c`;
    SDL3 native support covers MFi/PS/Xbox controllers and iPad keyboards.
4.4 iPad polish: logical-size letterboxing at 4:3 (the game is fixed
    320×240 — scaler choice matters on 2K screens), safe-area insets, audio
    session behavior (background/interruption), app icons/launch screen.
4.5 Distribution: dev signing + sideload docs; TestFlight only if licensing
    review clears it (see constraint in D8).

Exit criteria: playable on a physical iPad with touch and/or controller;
suspend/resume safe (auto-save on `SDL_EVENT_DID_ENTER_BACKGROUND`).

## Phase 5 — Ongoing modernization (non-blocking backlog)

- **Vendored single-header decoders (stb).** Owner request (2026-07-25):
  replace the external decode-only dependencies with `stb` equivalents.
  - `libpng` + `zlib` → `stb_image.h`. Only three files touch libpng
    (`libs/graphics/sdl/png2sdl.{c,h}`, `sdluio.c`) and the tree never
    *writes* a PNG — there is no `png_write_*` call anywhere — so a decoder
    is the whole requirement. zlib survives only if `UQM_ZIP` still needs it
    for the `.uqm` archives; check `libs/uio` before dropping it.
  - `libvorbis` + `libogg` → `stb_vorbis.c`. One consumer,
    `libs/sound/decoders/oggaud.c`, behind the `SoundDecoder` vtable, so the
    blast radius is a single file. Both pulldata (whole file) and pushdata
    (streaming) APIs exist; the streaming path is what `stream.c` wants.
  - Payoff is mostly build-side and lands on every platform at once: four
    FetchContent dependencies become two vendored files, the wasm artifact
    shrinks, and the iOS preset (4.1) stops needing third-party builds.
  - Watch: stb_vorbis is less tolerant of malformed streams than libvorbis
    and has no `ov_time_seek` equivalent — `oggaud.c`'s seek path needs
    checking against `stb_vorbis_seek_frame`. Keep the old decoders behind a
    CMake option for one release so a bad file can be A/B'd.
  - Sequencing: independent of Phases 2–4, so schedule whenever it is
    convenient. Pulling it before Phase 4 is attractive if the iOS
    dependency setup turns out to be the annoying part.
- C11/C17 cleanup passes (`port.h` shim removal, fixed-width types).
- Replace x86-asm scalers with portable/SIMD-intrinsic versions or delete.
- Netplay over WebSocket/WebRTC (would also enable browser↔desktop melee).
- Upstreaming: offer CMake + SDL3 + THREADLIB_NONE patches to SourceForge
  upstream / coordinate with MegaMod.

---

## Risk register

| Risk | Phase | Mitigation |
|---|---|---|
| SDL3 migration regressions (subtle input/audio behavior) | 1 | keep SDL2 branch point tagged; QA checklist per subsystem |
| Subtle timing regressions in converted animations/cutscenes | 2 | incremental landing on a playable threaded build; A/B vs pre-conversion tag; per-subsystem checks |
| Hidden blocking site missed before the flip | 2 | CI burn-down grep is the gate (`unthread.md` §7.5) |
| A subsystem resists flattening | 2 | coroutine fallback recorded (`unthread.md` §9) |
| Safari AudioWorklet/audio-unlock quirks | 3 | much lower risk without SAB; OpenAL fallback (D5) |
| iOS licensing (GPL + NC content) | 4 | sideload-first distribution; no App Store plan |
| Content licensing for public hosting (NC clause) | 3 | host content on project infra with attribution; same content the project itself distributes |
