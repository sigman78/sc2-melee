# Working in this repo

A C++23 rewrite of the Ur-Quan Masters melee. Read `README.md` first for
layout, then `docs/README.md` for where the reasoning lives.

## The gates — run these, believe these

	export PATH="/c/utils/scoop/apps/msys2/current/mingw64/bin:$PATH"   # bash on Windows, or gcc exits 1 silently
	cmake --build build 2>&1 | grep -E "error|FAILED"
	./build/tests/replay_test.exe --compare tests/baselines/replay.base   # "all 32 battles matched exactly"
	ctest --test-dir build

**The replay baseline is the contract.** Any change to `src/sim/` that is
meant to be behaviour-preserving must leave it bit-exact. It is the thing
that makes large refactors safe here, so:

- **Never re-record the baseline unless asked.** `replay_test --trace` *writes to its file
  argument* — only `--compare` and `--compare-first` may ever be pointed at
  `tests/baselines/replay.base`.
- **Never change a test expectation to make something pass.** Rewriting *how*
  a test reaches a value is fine; changing what it asserts is not.
- A divergence is information. Localise it with `--compare-first 4` and fix
  the cause, or stop and report it.

Builds are Debug, so `assert` is live: the suite exercises every assertion.

## What the code is

Composition is the whole description of a thing — there is no `Element`, no
base struct, no kind enum. An entity *is* which components it carries. Before
touching `src/sim/`, read `docs/component-map.md`; it lists every component,
what question it answers, and the worked composition of each thing in a
battle.

Two invariants that are easy to break and expensive to debug:

- `Lifetime` and `Doomed` are **not** redundant. `Doomed` means "the death
  response already ran"; `Lifetime{0}` without it is a live state the
  piercing pair and the flame's linger both depend on. The state table is in
  `docs/component-map.md`.
- Walk order is `(layer, seq)`, kept by a sorted `Order` pool. Layer order is
  gameplay; within-layer order is *determinism* — measured, not assumed
  (`src/docs/review-008` §6).

## Style

`docs/cpp-conventions.md` is the rule set. The two that come up most:

- **Comments are citations and constraints, not stories.** A citation to the
  C this came from (`process.c:361-627`), or an invariant a reader could
  violate. Three lines. Names and signatures carry the rest. Design
  narrative, refactor history and "this used to be X" belong in `docs/`.
- **Queries declare their read-set.** A pass names its components in
  `each<Ts...>`/`eachOrdered<Ts...>`; presence filters go in the query
  (`entt::exclude`), value tests in the body. `find<T>` inside a loop is for
  a conditional read of *another* entity — otherwise use `get<T>`, which
  asserts.

Prefer `i32/u32/u64/usize`. Tabs, matching the surrounding file.

## The C source

This is a port and it cites its source: `process.c:361-627`,
`tactrans.c:757-770`. Those line numbers resolve against a pinned revision of
the original tree — see `docs/reference.md`. Keep `sc2-uqm` checked out
beside this repo; that also supplies the content the content-dependent tests
read (`UQM2_CONTENT_DIR` defaults to `../sc2-uqm/sc2/content`).

`sim`, `replay` and `engine` need no content at all.

## Conventions that will bite

- Entity zero is a live entity. A default-constructed `EntityId` is a
  landmine; the empty value is `kNoEntity`.
- clangd in-editor errors about missing `engine/core/Types.hpp` or unknown
  `i32` are include-path noise. The CMake build is the truth.
- CRLF warnings on commit are repo policy noise.
- Commit subjects are lowercase and scoped: `sim:`, `app:`, `docs:`,
  `build:`. Bodies name what moved and what died.
