# uqm2 — The Ur-Quan Masters, rewritten

A C++23 rewrite of the Ur-Quan Masters battle simulation, extracted from the
[sc2-uqm](https://github.com/sigman78/sc2-uqm) tree where it grew alongside
the original C game.

What runs today: a melee — Earthling Cruiser against Ilwrath Avenger — on a
real window, with a deterministic simulation, a replay harness that pins 32
battles bit-exactly, and a content pipeline that reads the original game's
formats.

## Building

	cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
	cmake --build build
	ctest --test-dir build

SDL3 and EnTT are fetched at configure time; `spng` and `miniz` are vendored
(`third_party/README.md`). Nothing else is needed to build and test the
simulation.

## Content

The game's data files are **not** in this repository — they are 172 MB and
they belong to the original tree. Only some tests and tools need them:

| Needs content | Does not |
| --- | --- |
| `content`, `game`, `browse_inventory`, `browse_sheet`, `browse_font` | `sim`, `replay`, `engine` |

So a plain clone builds every target and runs the simulation suite green with
no content at all. To enable the rest, point at a content tree:

	cmake -S . -B build -DUQM2_CONTENT_DIR=/path/to/uqm/content

It defaults to `../sc2-uqm/sc2/content`, i.e. a sibling checkout of the
original tree — which is also where the C sources referenced throughout this
codebase live. See `docs/reference.md`.

## Layout

	src/sim/        the simulation: deterministic, no I/O, no wall clock
	src/engine/     content parsing, rendering, audio, platform
	src/game/       ship catalog, camera, sprite sets
	src/app/melee/  the melee application
	tests/          standalone programs, registered with CTest
	docs/           architecture and the review log
	third_party/    vendored spng + miniz

Start with `docs/README.md`; `docs/sim-architecture.md` and
`docs/component-map.md` describe the design, and `src/docs/review-001..009`
records how it got there.
