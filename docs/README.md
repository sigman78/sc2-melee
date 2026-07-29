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

## The C++ rewrite (`src/`)

Source comments in `src/` are deliberately terse — a citation to the C they
came from, and any invariant a reader could violate. **The reasoning lives
here**, not in the code.

| Doc | Contents |
|---|---|
| [sim-architecture.md](sim-architecture.md) | What kind of ECS this is: composition adopted, storage bounded, traversal order as data, the promotion rule |
| [component-map.md](component-map.md) | Every registry type grouped by the question it answers, the composition of each thing a battle contains, and the duplication audit |
| [cpp-conventions.md](cpp-conventions.md) | Style rules the rewrite holds itself to |
| `src/docs/design-notes.md` | Numbered design decisions and the divergence ledger — every place the port deliberately differs from the C |
| `src/docs/review-001..009` | The reviews, in order. Each records what changed, what was rejected and why, and what the gates proved. This is where refactor history belongs |

Analysis snapshot: 2026-07-25 @ `d6583f225`.
