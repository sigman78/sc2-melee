# UQM Modernization & Porting Docs

Goal: bring this vanilla Ur-Quan Masters tree (SourceForge `sc2/uqm`) up to
date on modern tooling, then ship (1) a WASM in-browser build and (2) an iOS
iPad build.

| Doc | Contents |
|---|---|
| [current-state.md](current-state.md) | Codebase facts: build system, SDL status, threading/main loop, audio/video, file I/O, content, netplay |
| [prior-art.md](prior-art.md) | Existing ports/forks worth learning from (intgr/uqm-wasm, MegaMod, iOS/Android attempts) + licensing constraints |
| [options.md](options.md) | Options weighed per decision area, with assessments |
| [decisions.md](decisions.md) | ADR log (mostly *proposed*, awaiting sign-off) + open questions |
| [c-making.md](c-making.md) | Phase 0 execution plan: CMake replacing the shell build, pruned of dead platforms, find-or-fetch deps, presets, CI, parity gate |
| [unthread.md](unthread.md) | The de-threading map: inventory, direct-flattening strategy (DOS-style flat loop, no coroutines), conversion patterns, audio changes, burn-down plan |
| [plan.md](plan.md) | Phased roadmap: 0 CMake/CI → 1 SDL3 → 2 un-thread → 3 WASM → 4 iOS → 5 backlog; risk register |

Analysis snapshot: 2026-07-25 @ `d6583f225`.
