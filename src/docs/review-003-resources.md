# Review 003 — resources keyed all the way down

The overhaul SiGMan asked for: sprites, sounds and the stand-in collision
masks reached by key, with the keys living in the data-driven ship and game
definitions rather than in code that loads things. Verdict up front: **the
bottom layer already was data-driven** — `game::Resources` caches by uqm.rmp
id (design-notes D10) — **and everything above it was not**. The work is
naming where keys were hardcoded and moving each into a definition. Staged
R1–R5; R1 is executed below.

## 1. Where the keys were hardcoded

Four sites, one per kind of leak:

1. **Load-time ids in code** — Assets.cpp named nine GFXRES/SNDRES ids as
   string literals and stored the results into *named `Game` members*
   (`g.cruiser`, `g.nuke`, `g.flame`…). Adding a ship meant editing the
   struct, the loader and every consumer.
2. **Roster baked into presentation dispatch** — `visualFor` (Draw.cpp) and
   `playStepSounds` (Sound.cpp) picked art by `playerNr == 0 ? cruiser :
   avenger`. Collapses at the third ship.
3. **Mask wiring by hand** — four per-ship lines in Assets.cpp copied
   `SpriteSet::masks` into the spec's `facingMasks`/`weapon.masks`.
4. **Placeholder masks as members** — `Game::shipMask/shotMask/rockMask/
   planetMask` (`block(w, h)`) with per-callsite fallback logic in Game.cpp.

`ShipSpec` itself stays content-free by design — sim/ never reads files —
so the keys' home is *beside* the spec, not inside it.

## 2. The shape

`game/Ships.hpp`: a `ShipDef` is one entry — the key setup picks it by, the
sim descriptor, and the content its presentation needs:

    ShipDef{.key = "earthling.cruiser",
            .spec = &sim::earthlingCruiser(),
            .art{.ship = "ship.earthling.graphics.human.large",
                 .weapon = "ship.earthling.graphics.saturn.large",
                 .sounds = "ship.earthling.sounds"}}

`ShipArt` is the C's `SHIP_DATA` (ship.h:69-81) minus the three sizes
collapsed to `-big` and the pieces melee does not draw yet. `shipCatalog()`
returns every ship the game knows; `findShip(key)` resolves one. A code
literal today, the shape a ship file parses into later (review-002 §3) —
which is why it is data all the way down and not a class.

`game/Melee.hpp`: `kMeleeArt`, the melee mode's non-ship keys — asteroid,
blast, boom, planet, stars, battle sounds. The mode owns what the arena
draws; a ShipDef owns what a ship brings.

Both live in the headless half of game/ (compiled into `uqm2_content`), so
tests reach them without linking SDL. `Resources` and `SpriteSet` stay in
`uqm2_platform` with the window and device they need.

## 3. The staged plan

| Stage | What | Status |
| --- | --- | --- |
| R1 | `ShipDef`/`ShipArt` catalog + `MeleeArt`; Assets.cpp consumes definitions; `game_test` pins every named id to uqm.rmp | **done** |
| R2 | `Game` loses its named asset members; a per-player roster of `Borrowed<const ShipDef>`; `visualFor`/sounds resolve through it | next |
| R3 | `materialize(def, content, window) -> ShipSpec`: the spec copy plus its content-derived masks, one function instead of hand lines | |
| R4 | Placeholders served by `Resources` on a miss — generated rect frames + `block` masks — retiring the `Game` mask members and per-callsite fallbacks | |
| R5 | Sound slots named (`ShipSound::Primary`, `BattleSound::Shipdies`…): the .snd line order is a content contract, written once | |

R2–R4 are mechanical sweeps against this review; R5 is a rename. All are
behavior-preserving under the existing suites, same proof mechanism as
review-002's E-stages.

## 4. R1 as executed

- `game/Ships.hpp` + `Ships.cpp` — `ShipArt`, `ShipDef`, `shipCatalog()`,
  `findShip()`. `game/Melee.hpp` — `MeleeArt`, `kMeleeArt`.
- Assets.cpp: the roster is two `findShip` calls; every `load()`/
  `loadSounds()` takes its id from a definition; the spec copies come from
  `def.spec`. No resource id remains as a string literal in app/.
- `tests/game_test.cpp` (CTest `game`): every id the catalog and `kMeleeArt`
  name must resolve in uqm.rmp, keys are unique and round-trip through
  `findShip`, specs are `valid()`. With `browse_inventory` (rmp → file)
  this closes the chain **definition → rmp → file**: a key typo fails the
  suite instead of drawing a silent rectangle at runtime.

Verified: full rebuild of the `core` preset, 7/7 ctest green, and a driven
run — both ships render their sprites, the starfield draws its cels, the
Avenger's flame fires (energy 16 → 14 on the HUD). Ids byte-identical to
before; only their source moved.

## 5. Found along the way

- **build/core had not been rebuilt since before E4 landed** (binaries
  00:09, the literals commit 05:00), so "suites green" at E4 had not been
  proved against GCC's `-Werror` under this preset.
- GCC's `-Wextra` implies `-Wmissing-field-initializers`, which fires on
  every partial designated-initializer literal — the exact idiom the spec
  literals (and now the catalog) are built on, where an omitted field *is*
  the meaningful default. Disabled for `uqm2_content` with a comment; the
  alternative, spelling out every defaulted field, fights the style E4
  committed to and M2 will multiply.
- The MinGW silent-death trap (compiler exits 1 with no message when
  mingw64/bin is not on PATH) struck again and is already documented in the
  toolchain notes; the fix is the PATH export before any build.

## 6. Out-takes

- No registry machinery: a catalog literal is enough until a file format or
  modding forces more (review-002 §3's deferral stands).
- The ship key namespace (`earthling.cruiser`) is ours, deliberately not a
  uqm.rmp id: rmp ids name files, ship keys name definitions, and the two
  vocabularies should not be confusable.
- `MeleeArt.planet` stays one fixed type until melee setup exists to choose
  per battle (load_gravity_well, cons_res.c:52-82) — the note moved from
  Assets.cpp to the field it describes.
