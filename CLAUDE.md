# Working in this repo

A C++23 rewrite of the Ur-Quan Masters melee. Read `README.md` first for
layout, then `docs/README.md` for where the reasoning lives.

## After cloning

Two settings are per-clone git config, so a fresh checkout does not have
them and the pre-commit hook silently never runs until it does:

	git config core.hooksPath .githooks
	git config blame.ignoreRevsFile .git-blame-ignore-revs

	cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON

`UQM2_CONTENT_DIR` defaults to `../sc2-uqm/sc2/content` — a sibling
checkout of the original tree, which is also where the C sources cited
throughout this code live (`docs/reference.md`). Without it the five
content tests skip and everything else still runs.

## Useful to remember

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
- A divergence is information. Localise it with `--compare-first 4` and fix
  the cause, or stop and report it.

Builds are Debug, so `assert` is live: the suite exercises every assertion.

- `Lifetime` and `Doomed` are **not** redundant. `Doomed` means "the death
  response already ran"; `Lifetime{0}` without it is a live state the
  piercing pair and the flame's linger both depend on. The state table is in
  `docs/component-map.md`. (remark: for now!)
- Walk order is `(layer, seq)`, kept by a sorted `Order` pool. Layer order is
  gameplay; within-layer order is *determinism* — measured, not assumed
  (`src/docs/review-008` §6). (remark: for now!)


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

## Open, and deliberately so

Decisions already taken with reasons recorded — do not silently reverse them:

- **Walk order is a free parameter.** Reversing `seq` within a layer changed
  no outcome across all 32 battles (`src/docs/review-008` §6). `seq` stays
  for determinism, not gameplay. Changing the order costs one baseline
  re-record and nothing else.
- **`Stagger{turn,thrust}` and `Cooldowns{weapon,special}` were rejected**
  (`src/docs/review-009` §1): nothing operates on either pair, so grouping
  them would name a thing that does not exist.
- **PCH was measured and dropped.** ~2.4% on the file that should benefit
  most; the cost is template instantiation, which a PCH cannot touch.
- **`-gsplit-dwarf` is poison here** — it takes a debug melee from 55 MB to
  39 MB and produces executables this MinGW toolchain cannot run. `-g1`
  works and is a further 40%, at the cost of variable inspection.
