# The Ur-Quan Masters â game-logic rewrite plan (`rewrite/core`)

## Goal

One C++20 codebase, single-threaded, SDL3, WASM-first, that plays the same game from the same
content packs. The existing C is read as a specification and discarded. Success is a player
saying "this is Star Control 2"; it is not a matching frame counter. `docs/rewrite-plan.md`
stays in the tree for its findings; its recommendation to port `ships/` and `comm/` verbatim is
withdrawn.

---

## What the game actually is

**One entity simulation, four games on top of it, a dialogue interpreter, and a bag of screens** â
wired through one mutable global struct. `RedrawQueue` (`process.c:1013`) is the simulation step,
not a redraw, and it is shared by melee, hyperspace (`hyper.c:1368`), interplanetary
(`ipdisp.c:567`) and â via a fork â the lander (`lander.c:1003-1008`). `init.c:190-198` vs
`:226-233` is the proof that hyperspace is a one-ship battle on the same queue. Building melee
buys hyperspace's loop.

Four structural findings drive every decision below.

**1. The engine owns the primary weapon and nothing else.** `ship.c:294-340` runs the entire
primary lifecycle. Its total handling of SPECIAL is `if (special_counter) --special_counter;`
(`ship.c:342-343`). `special_energy_cost` and `special_wait` are never read outside `ships/`.
Every special's cost, cooldown and effect is per-ship code. That one asymmetry explains most of
`ships/`.

**2. Weapon spawn is called speculatively, by both sides, every frame.** `cyborg.c:339-410`
copies the ship element, advances a tick, calls `init_weapon_func` on the copy, and frees the
results. Writes through `StarShipPtr` survive. I verified the worst case: `umgah.c:330-341`,
inside `initialize_cone`, calls `SetCustomShipData` â `HFree` + `HMalloc` and rewrites
`ship_data.special[0]`. Every AI lookahead frame heap-churns the Umgah, with no compensating
hack (Orz has one, `orz.c:249-253`). That is a live defect in the shipped build and the
strongest argument for pure spawn descriptors.

**3. Comm's vocabulary is genuinely tiny, its density is not uniform.** Across 27 race files:
`NPCPhrase` 1756, `PLAYER_SAID` 1125, `Response` 954, `GET_GAME_STATE` 802, `SET_GAME_STATE` 691,
`PHRASE_ENABLED` 540, `DISABLE_PHRASE` 329. Nine verbs are ~96% of the corpus. **The comm report's
"96.9% expressible" figure does not survive its critique** â its script counted calls to
in-file helpers (`StripShip`, `AnalyzeCondition`, `CheckBulletins`, `DeltaCredit`, `sayCoord`)
as expressible. Drop the number; the token census carries the argument on its own.

**4. Content already carries the names we need.** `getstr.c:270-303` parses `#(NAME)` labels and
`:229/:457` builds a hash table keyed by them. `GetStringByName` (`strings.c:340`) has zero
callers. Phrase binding is by ordinal today (`commglue.c:78-84`), so 27 `strings.h` files and
3,451 identifiers must stay in lockstep with `.txt` line order with no check. Name binding is
already paid for.

---

## Ship model

**Three layers: a ship *file* (data), a component *library* (C++), a tactics *class* (C++).**
No DSL for AI â `blackurq.c:432`, `chenjesu.c:491` and `zoqfot.c:331` call `ship_weapons` and
`PlotIntercept` from inside their tactics, so any adequate DSL would have to expose the collision
lookahead. That settles it.

Four repairs to the ships report, all of which the critique earned:

- **`ShipData` must be constructible in code, not only parsed.** Verified at
  `shofixti.c:461-517`: when `IN_ENCOUNTER && !SHOFIXTI_RECRUITED`, `init_shofixti` copies the
  descriptor, swaps in `OLDSHOF_*` art, nulls the glory-device sprites, sets `weapon_wait = 10`,
  and then runs the VUX limpet loop `NUM_LIMPETS` times against its own stats â with a comment
  tying it to `MIN_THRUST_INCREMENT in vux.c`. `sis_ship.c:881-990` computes its whole descriptor
  from flagship inventory. So: `ShipData` is an immutable *value*; a loaded file is one way to
  make one, a builder function is another.
- **The weapon block needs a hold-to-charge kind.** Four melee primaries are not
  projectile-or-beam: melnorme pump-up (`melnorme.c:311-346`, damage `PUMPUP_DAMAGE << charge`),
  chenjesu crystal (`chenjesu.c:185-198`, `++life_span` while held, fragments on release),
  Kohr-Ah buzzsaw (`blackurq.c:277-289`, `life_span >>= 1` on release), umgah cone
  (`umgah.c:306-355`, a persistent melee-range hitbox). Add `kind = charge` with
  `whileHeld{...}` and `onRelease{fragment|decay|expire}`.
- **Tactics may call the brain more than once, over different worlds.** Verified at
  `melnorme.c:575` and `:597`: two `ship_intelligence` calls in one tick, the second with
  `initialize_confusion` installed as `init_weapon_func` and a truncated concern list
  (`ENEMY_SHIP_INDEX + 1`), used to make the shared brain *aim the special*. A
  `decide() { return Brain::baseline(...); }` interface cannot express that.
- **Modifiers are a stack on the target ship.** VUX limpets write the enemy's stats
  (`vux.c:157-190`) including `cyborg_control.ManeuverabilityIndex = 0`; Pkunk clears them by
  hard-coded literals (`pkunk.c:286-291`); Shofixti's damaged variant re-derives them. One
  `StatModifier` type kills all three copies.

### Interfaces

```cpp
struct ShipData {                    // immutable value; parsed OR built
  Stats base; Art art; WeaponSpec weapon; SpecialSpec special; AiHints ai;
};
struct Spawn { ... };                                     // pure value descriptor
std::vector<Spawn> spawnPrimary(const ShipView&);         // no allocation, no writes
class ShipTactics {
  virtual Intent decide(ShipView&, Concerns&, Brain&) = 0; // Brain callable 0..N times
};                                    // ShipView exposes liveSpawns() and intercept(a,b)
```

Five engine primitives are prerequisites, not nice-to-haves: parameterised
`thrust(Element&, facing, profile)`; `applyImpulse` with **speed flags derived from |v| vs
max_thrust** rather than hand-patched (kills `chmmr.c:398-409`, `druuge.c:266`,
`mmrnmhrm.c:436-450`); real element tags (kills `shofixti.c:251-253`'s
`#include "../orz/orz.h"` and `cyborg.c:1222-1227`'s frame-pointer-as-type-tag); derived threat
advertisement (`orz.c:881-885` â 3 lines; makes `chmmr.c:479/664`'s accidental refcount
unwritable); pure spawn descriptors. If ships get built before these land, every ship
re-imports the save/override/restore idiom and we have transliterated after all.

### Worked example â Supox, from `sc2/src/uqm/ships/supox/supox.c` (288 lines)

```toml
id = "supox"  name = "Blade"  cost = 16  mass = 4
crew   = { max = 12 }
energy = { max = 16, regen = 1, regenWait = 4 }
thrust = { max = 40, increment = 8, wait = 0 }
turn   = { wait = 1 }
art    = { hull = "SUPOX_{BIG,MED,SML}", captain = "SUPOX_CAPTAIN",
           weapon = "GOB_{BIG,MED,SML}", victory = "SUPOX_VICTORY_SONG",
           sounds = "SUPOX_SHIP_SOUNDS" }
campaign = { sphereRadius = 333, homeworld = [7468, 9246] }

[weapon]                    # was initialize_horn, supox.c:187-209
kind = "projectile"  cost = 1  cooldown = 2
muzzle = { from = "hull", forward = 23 }
speed = 30  life = 10  hp = 1  damage = 1  blastOffset = 2
collides = "ignoreSimilar"

[special]                   # was supox_preprocess, supox.c:212-273
component = "OmniThrust"
cost = 0                    # NOTE: supox.c:218-220 comments out the DeltaEnergy call.
                            # The Supox special is free in the shipped game. Kept.
cooldown = 0
params = { directions = ["back", "left", "right", "backLeft", "backRight"],
           consumesTurnAndThrust = true }

[ai] weaponRange = 150  threatArcs = ["fore"]  tactics = "supox"
```

`OmniThrust` is one component, shared with Umgah's retro-thrust: pick a facing delta from held
keys, call `thrust(element, facing + delta, profile)`. Twelve lines. It is sixty in the C
(`supox.c:242-271`) only because `inertial_thrust` reads facing from `STARSHIP`, so the ship must
save `ShipFacing`, overwrite it, hand-merge the returned status flags, and restore. Engine
primitive #1 deletes that shape everywhere. `supox_intelligence` (`supox.c:124-186`) stays as
~60 lines of C++ adjusting `Concerns` before the brain and `Intent` after.

**Estimate for the layer:** 11,413 non-blank lines of `ships/` â roughly 900 lines of ship files
plus ~3,000 lines of C++ (â15 components, ~19 one-offs, 27 tactics classes). I would defend the
shape firmly and the ratio to Â±40%. Three of the ships report's five estimate rows had no stated
derivation; treat the total as a direction, not a budget.

---

## Dialogue model

**A declarative node format, a curated host-verb surface, and four game systems lifted out of
`comm/` entirely** because they were never dialogue: Melnorme trading (`melnorm.c:200-757`,
`:960-1178` DoBuy â which the comm report's extraction ranges skipped â and `:1179-1298` DoSell),
starbase services (`starbas.c:614-783`, `:1065+`, `:1370+`, `:1758-1820`), fleet/diplomacy
(already in `build.c`/`gameev.c`), and encounter presentation.

No embedded scripting VM. It re-imports arbitrary code into the content layer and adds a second
GC to the WASM build. A small text format with a pure expression sublanguage for guards,
compiled at build time, parsed directly in dev builds for hot reload.

Four repairs the critique earned, all verified in the tree:

- **`GLOBAL_FLAGS_AND_DATA` is not a call parameter.** `commglue.c:391-395` uses bit 7 to decide
  *which race file runs* (`init_spathi_comm` vs `init_spahome_comm`) â consumed before the
  conversation exists. `comm.c:1866-1874` re-reads it after a load to pick the conversation, and
  the clear at `comm.c:1814` is skipped on `CHECK_LOAD`. It is a persisted pre-conversation
  selector. Model it as `encounterSite`, saved.
- **A handler may replace its node's whole offer list.** `supoxc.c:383-394`: the `whats_ultron`
  branch emits, writes two state fields, registers two offers and `return`s, bypassing the node's
  entire offer computation at `:420-529`. Five sites do this. The node shape must allow
  `offers { ... }` inside a handler, terminating.
- **The response columns are gated, not merely ordered.** In `NeutralSupox` every arm of the
  `SUPOX_STACK1` switch also writes `pStr[1] = 0` (`supoxc.c:438-470`), and there is no `case 5` â
  so column 1's topic is unreachable until the player has exhausted `i_am â my_ship â
  from_alliance â are_you_copying â why_copy`. `offer group recent-first` cannot say that.
- **`construct_response` is a template, not a fixed set.** `commglue.c:260-312` concatenates an
  alternating (ref, string) list of arbitrary length; `shofixt.c:428` declares two buffers because
  one node builds two live responses at once. Also required: `when speech` emission branches
  (`starbas.c:1670-1681` emits different phrase *names* and counts), and declared ordered runs for
  arithmetic refs (`ENUMERATE_ONE + n` at `melnorm.c:749`, the numeral grammar at
  `commglue.c:180-190`, alliance names at `:60-70`).

### Worked example â `NeutralSupox`, from `sc2/src/uqm/comm/supox/supoxc.c:307-530`

```
node neutralSupox {
  columns [intro, species, utwig]     # rendered leastRecent-last; `advanced` leads

  on i_am           { say WE_ARE_SUPOX;    set supox.stack1 = 1; retire i_am }
  on my_ship        { say OUR_SHIP;        set supox.stack1 = 2; retire my_ship }
  on from_alliance  { say FROM_SUPOX;      set supox.stack1 = 3; retire from_alliance }
  on are_you_copying{ say YEAH_SORRY;      set supox.stack1 = 4 }
  on why_copy       { say SYMBIOTS;        set supox.stack1 = 5 }

  on tell_us_of_your_species  { say OUR_SPECIES;          set supox.stack2 = 1; advanced species }
  on plants_arent_intelligent { say PROVES_WERE_SPECIAL;  set supox.stack2 = 2 }

  on anyone_around_here     { say UTWIG_NEARBY;  set supox.warNews = 1; advanced utwig
                              do fleet.track(UTWIG) }          # StartSphereTracking
  on what_relation_to_utwig { say UTWIG_ALLIES;  set supox.warNews = 1; advanced utwig }
  on whats_wrong_with_utwig { say BROKE_ULTRON;  set supox.warNews = 2; advanced utwig }

  on whats_ultron {                              # supoxc.c:383-394 â replaces the offer list
    say TAKE_ULTRON
    set supox.warNews = 0;  set ultron.condition = taken
    offers { what_do_i_do_now -> farewell;  thanks_now_we_eat_you -> farewell }
  }

  on got_fixed_ultron        { say GOOD_GIVE_TO_UTWIG; set supox.ultronHelp = true }
  on look_i_repaired_lots    { say ALMOST_THERE;       set supox.ultronHelp = true }
  on look_i_slightly_repaired{ say GREAT_DO_MORE;      set supox.ultronHelp = true }
  on where_get_repairs       { say ANCIENT_RHYME;      set supox.ultronHelp = true }

  column intro when supox.stack1 == 5 -> hidden       # the pStr[1]=0 gate, made explicit
  column intro match supox.stack1 {
    0 -> offer i_am           as "{i_am0}{sis.commanderName}{i_am1}"
    1 -> offer my_ship        as "{my_ship0}{sis.shipName}{my_ship1}"
    2 -> offer from_alliance  as "{from_alliance0}{allianceName}{from_alliance1}"
    3 -> offer are_you_copying
    4 -> offer why_copy
  }
  column species match supox.stack2 {
    0 -> offer tell_us_of_your_species
    1 -> offer plants_arent_intelligent
  }
  column utwig when ultron.condition == none  match supox.warNews {
    0 -> offer (utwig.visits or utwig.homeVisits or bomb.visits)
              ? what_relation_to_utwig : anyone_around_here
    1 -> offer whats_wrong_with_utwig
    2 -> offer whats_ultron
  }
  when not supox.ultronHelp  match ultron.condition {
    taken   -> offer where_get_repairs
    partial -> offer look_i_slightly_repaired
    nearly  -> offer look_i_repaired_lots
    fixed   -> offer got_fixed_ultron
  }
  offer bye_neutral -> farewell
}
```

Note what the format buys: the `pStr[1] = 0` gate becomes a `-> hidden` line instead of an
invisible side effect of five switch arms; `LastStack` becomes `advanced <column>`, declared where
it happens; `ULTRON_CONDITION` becomes an enum instead of a magic 0-4; and the two-line
`construct_response`/`DoResponsePhrase` split (`supoxc.c:485-491`) collapses into one `as`
template. `retire`/`fresh` is per-conversation, not saved â `comm.c:1523/1649` load and destroy
the phrase table each visit, which is also the constraint that forbids caching it in the rewrite.

**Host verbs**: ~30 typed, versioned entries with effect classes (`ask`/`do`/`draws`). Systems
return *structured results* the script narrates: `melnorme.rescueOffer()` returns
`{fuel, tanksFull, modules[]}` and the script writes the enumeration loop that
`melnorm.c:743-752` hardcodes.

**State**: keep all 447 names, drop the packing. Generated named fields, typed
(`bool`, `0..N`, enum), namespaced by owner (255 of 447 fields are comm-private). **Fold the
split-word encodings** the architecture critique found â `MELNORME_CREDIT0/1`,
`RAINBOW_WORLD0/1`, `CREW_PURCHASED0/1`, `CREW_SOLD_TO_DRUUGE0/1` (whose halves are not even
adjacent), `STARBASE_BULLETS0..3` (a 32-bit mask, not a GRPOFFS) â into single values;
transliterating them enshrines the byte-addressing. The generator must also emit an indexâfield
map, because three sites index state by computed offset (`save.c:713-726`, `load.c:594`,
`genmel.c:68-111`) and `save.c`'s loop additionally requires the GRPOFFS block to stay the tail of
the enum. Fold GRPOFFS into `BattleGroups` and rewrite those two loops together.

---

## Architecture

```
platform/  SDL3, filesystem, time.        engine/  render, audio, content, input, Rng, Task<>
sim/       deterministic; no I/O, no wall clock, no globals
game/      modes, story, universe, dialogue VM, saves
app/       main + iterate()
```
CI greps the arrows. Note the sim's purity **is not already there**: `ship.c:176-192` draws the
captain's window and calls `BattleSong` from inside `ship_preprocess`, and `ships/` has 117 frame
API calls, 101 `DisplayArray` writes and 41 sound calls. That is module-one work, not an
inherited property.

- **Ordered entity list, not an arena.** `disp_q` is spliced into the middle: 20 `InsertElement`
  sites, ~13 at the head. `pkunk.c:503-512` documents why â the phoenix is head-inserted so it
  preprocesses before the dead Pkunk's `death_func` runs. Arena slot order is arbitrary after the
  first free-list reuse. Use a payload arena for stable ids **plus** an explicit ordered id list
  with `pushFront`/`insertAfter`.
- **Modes, not screens.** `Transition` is a returned value (`Push`/`Pop`/`Replace`), so nested
  screens resume in `onChildResult` rather than after a recursive `DoInput`. Pause/exit-confirm
  (`gameinp.c:236-243`, modal loops inside input sampling) become transitions the input layer
  requests. `CurrentActivity`'s low byte becomes the mode stack; its 8 relational tests
  (`process.c:219,251`, `init.c:117`, `ship.c:185,272,278,348`, `process.c:348`) become two named
  predicates, `isShipBattle()` and `isFreeFlight()`.
- **Coroutines for sequences.** `Task<>`/`co_await` replaces `libs/coroutine.h`'s Duff device and
  the single global `responseResumePc` (`commresponse.h:52-72`), deleting the "locals don't
  survive a yield" bug class and the ~15 hand-hoisted file statics. Compiler-generated heap
  frames â no Asyncify, no JSPI.
- **Pacing.** `ONE_SECOND == 840`, `BATTLE_FRAME_RATE == 840/24 == 35` ticks == 41.67 ms. Use
  `dueAt += period` with bounded catch-up. A naive `if (now >= next + period)` on a 60 Hz rAF loop
  lands on the third 16.7 ms tick â 20 Hz, a 17% global slowdown, browser-only, invisible on
  desktop because nothing in the tree vsyncs. Assert achieved battle Hz in CI, native and node.
- **Input** carries `held` plus a sticky `pressed` accumulator drained only when a gated step
  runs. Sampling edges at display rate for a 24 Hz mode is the bug that already cost a session
  (`docs/unthread.md Â§7a`).
- **Named RNG streams**, owned, seeded per battle from the scenario record. Presentation never
  draws from the sim stream (`commanim.c:50,59,67,90` currently does, on a wall-clock schedule).
  This is a deliberate behaviour change; record it.
- **Sound and HUD are declared outputs of `step()`**, not calls from within it: an event list
  keyed by `EntityId`, and a `HudDecal` list. `ModifySilhouette` (`weapon.c:249-300`) is the
  forcing case â a collision handler that rejection-samples the RNG against a HUD sprite's alpha
  mask and draws into the status panel. It belongs in neither `sim/` nor `engine/` as written.
- **`sim/` needs frame *identity*, not just masks.** Facing is stored in the FRAME index and read
  back 32 times, including `human.c:133` on the Earthling Cruiser. Carve out frame-set metadata
  (index, count, hotspot, mask) as a sim-visible type.
- **Saves**: one templated `Archive`, chunked as today (`save.c:733-813` â the loader already
  skips unknown chunks). Story chunk is nameâvalue. Save the resume point and the RNG stream
  positions explicitly. Legacy `uqmsave.NN` import, read-only, ~400 lines â kept as a forcing
  function: *if the new model cannot represent an old save, the model is wrong.* That import will
  also force `STARINFO` and `RANDGRPINFO` (`state.c:44-49`) into `Game`, which no report gave a
  home.

---

## Verification

No CRC. No frame-exact gate. Four instruments, in the order they must exist.

**1. Dialogue transcript oracle â build it before touching `comm/`.** Instrument the current
build at `NPCPhrase_cb`, `DoResponsePhrase` (`comm.c:1424`), `setGameState` (`globdata.c:62`,
which already logs `#SName`) and `DISABLE_PHRASE`. For a fixed (initial state vector, response
index sequence), record: phrases emitted in order, offers presented **in order** (12 files
hand-reorder the menu), state deltas by name, exit disposition. No timing, no pixels. Freeze as
fixtures. The new engine replays and must match. **The blocker is already solved:**
`UQM_DEBUG_COMM` was *added* at `dbd811362` and reverted at `4ac011772` â I verified both commit
subjects. `git revert 4ac011772` is a one-command prerequisite, not a risk. Fixtures must also
pin the speech setting and carry the whole `SIS_STATE`, because `getStripRandomSeed`
(`melnorm.c:559-574`) seeds a private RNG from flagship position and module slots.

**2. Content alignment checks in CI, from commit 1.** Preprocessor-aware enumâ`.txt` alignment
per race and per overlay (`pkunk/strings.h:103-107`'s `#if 0` block breaks any naive parse and
would silently shift 103 phrases). Extend it to `.ts` timestamps: `getstr.c:338-347` discards
*every* timestamp for a race behind one warning on a single name mismatch â an entire alien's
subtitle sync dies silently. Plus resource-graph orphan/dangling reports.

**3. Melee round-robin on pinned scenarios.** Pin explicit `{x, y, facing}` per ship **and the
asteroid field and planet** â `init.c:224-233` puts 5 asteroids and 1 planet in every melee, and
`misc.c:63-70` has a second geometry-dependent RNG rejection loop, so a shared seed does not give
a shared start. Measure three things: win rate, match-length distribution (median + IQR +
timeout rate â nothing in the tree caps battle length), and paired disagreement `d`. **The
critique's fix on the baseline is correct and I adopt it:** perturb *state* (Â±1 world unit on a
start position, +1 on a `weapon_wait`), not the RNG stream. Ten of 25 melee ships never call
`TFB_Random`, and a stream perturbation lands on asteroid respawn, so it would measure the wrong
sensitivity. Tiers: per-PR 625Ã24 (catches only catastrophe, which is what per-PR should do);
nightly 625Ã400 with a BH-ranked delta list; per-milestone 625Ã2000. **The tournament's job is to
direct human attention, not to grant approval.** Harness self-test: a VUX-vs-anything battle that
terminates â `weapon.c:274-286` rejection-loops until `DrawablesIntersect` succeeds, and
`intersec.c:244-246` returns 0 with no context, so a naive headless mode hangs there rather than
producing wrong numbers.

**4. Property tests and formula characterisation.** Per component, written *from the C*: a limpet's
Nth application yields the documented stats; the glory device's damage at distance d; a marine
kills at most one crew per 12 frames. Then ~30 pure numeric formulas (thrust integration, tractor
magnitude, limpet degradation, `TrackShip` delta) diffed offline against the C functions. Copy
`TFB_Random` bit-for-bit (`random.c:61-70` is *not* ParkâMiller; it wraps on `DWORD` and returns
M+t+2) and the trig tables, with golden-vector asserts.

---

## Milestones

**M0 â two oracles and a viewer. Start tomorrow morning.** Concrete first hour:

1. `git revert 4ac011772` on `rewrite/core`; confirm `UQM_DEBUG_COMM` forces a conversation.
2. Add the four trace hooks to the old build behind `UQM_TRACE_COMM`; emit one line per event.
3. Capture and commit the Supox transcript set (`supoxc.c` â 5 nodes, 6 foreign statements) as
   `tests/fixtures/comm/supox/*.trace`. It is the smallest race that exercises `construct_response`,
   `GetAllianceName`, an offer-replacing handler, a column gate and a host verb.
4. Wire the preprocessor-aware phrase checker into CI as a hard gate (it passes today: 27 races,
   3,451 phrases, 0 mismatches).

Then the rest of the week: the C++ content library (`uqm.rmp`, `.ani` CRLF text, PNG via vendored
spng, `.ct` with its two shapes, `.fon`, `.txt`) plus a sprite/font/colormap browser. Every
byte-level surprise in the project lives here and this is the cheapest place to find it.

**M1 â vertical slice (the architecture gate).** Human Cruiser vs Ilwrath Avenger, two players at
one keyboard, asteroid field and planet, music and SFX, 24 Hz sim under a display-rate render,
deployed to a URL in the same CI run. Forces: swept per-pixel time-of-impact collision
(`intersec.c:33`), the ordered entity list, the pacing accumulator, the input accumulator, the
ship interface with a child-spawning special, streaming content residency. Fix `optMeleeScale`
(two different camera *and* sprite-LOD paths, a user option today) and the 320Ã240 world here or
never. Also in M1, not M5: spike the dialogue model against **Melnorme's `StripShip`** â not for
the state model, which will pass, but because it snapshots and rolls back the entire flagship
(`melnorm.c:529,590,635`) and reseeds its own RNG from galactic position for Bug #567. If the
format survives that, it survives the other 26.

**M2 â Super Melee, 25 ships, front end, AI.** Port `cyborg.c` as literally as C++ allows *first*:
it is the shared reference frame every per-ship tactic was tuned against, and rewriting it and the
ships simultaneously leaves no fixed point. Refactor it only once the win-rate matrix is stable.
Ships in easyâhard order; refuse to generalise a component until its second user exists.

**M3 â hyperspace, starmap, SIS.** Reuses M1's loop, but is 8,389 lines, not 6,000, and it is where
**persistent world state arrives** (`grpinfo.c`, 865 lines, is one of the three state files
`save.c` serialises). Pull the state model forward into M3; do not leave it to M5.

**M4 â solar systems, orbit, scan, lander.** 29 generator files, 5,797 lines, spread 671:70 â not
near-clones; they encode hand-placed content across 502 systems.

**M5 â dialogue, adventure state, events, save.** Convert the 21 low-foreign races first, extract
the four systems, then `melnorm`/`starbas`/`chmmrc`/`comandr` against the new verb surface. Delete
all 27 `strings.h`.

**M6 â starbase, outfitting, endgame, cutscenes.** Plus `gameopt.c`/`menu.c`/`flash.c` (~2,900
lines of always-on UI no report scheduled).

**No calendar.** The sequencing report's 35â43 weeks descends from a 25-hour diffstat that includes
whole-file CRLF re-emission, docs and tests, and that measured *editing to a known API*, not
authoring novel C++ from a spec that must first be read. Derive the schedule from M0+M1 actuals,
measuring work of the same kind. Keep `sc2/` building and in CI for the project's whole duration â
side-by-side comparison is the only oracle the feel questions have, and it costs almost nothing.

---

## What we accept losing

- **Frame-exactness, and therefore netplay v1.** Lockstep netplay (5,960 lines) is out of scope for
  the rewrite's first release. This must be decided *before* M1 fixes the loop model, because
  lockstep would constrain input delay, determinism and checksum cadence. Decided: out.
- **Save compatibility, one direction.** Old saves import; new saves do not export to the old
  format. `libs/decomp` (pre-0.7) is dropped.
- **RNG stream unity.** Presentation gets its own stream. Consequence: post-conversation RNG
  position is not reproducible against the old build by construction. Do not build tooling that
  assumes otherwise.
- **Ship "vtables" as mutable per-instance data.** `chmmr.c:773` deletes its own hook;
  `pkunk.c:282` reinstalls it. These become explicit state machines. Behaviour at the edges â
  which frame the hook stops firing â will differ.
- **Latent bugs, reproduced and annotated, not silently fixed.** Policy decided now:
  `supoxc.c:169`'s dead write and its cousins are reproduced with a comment citing the line,
  because a divergence discovered later is unattributable. Exceptions require a note in the
  changed node. Capture all fixtures from the *unmodified* C first.
- **Truncation semantics.** `setGameState` (`globdata.c:62-82`) masks the destination but not the
  value, so an over-wide write corrupts its neighbour. Typed fields make that a load-time error.
  Nothing appears to depend on the wrap, but it is a behaviour change, taken deliberately.
- **Match-outcome resolution moves out of ship files.** Three ships own it today â
  `shofixti.c:334-350` sets the winner, `pkunk.c:294` wins simultaneous destructions by
  reincarnating, `lastbat.c:335-337` ends the battle on campaign state. The match loop takes it,
  modelling "died but may still win" and "died but returning". Edge cases here are exactly the
  ones players notice; expect to tune them.
- **Feel, initially.** Every combat clock is a per-frame `BYTE` countdown (`races.h:147-157`);
  decrementing in the wrong phase shifts an effective rate by 1/N. Expect a tuning pass per ship
  and budget for it as the largest irreducibly-human block in the project.

---

## Open questions

1. **Modding contract.** May a mod *add* a ship and a race (needs a component registry and a
   resource-id namespace), or only retune existing ones? This decides whether the component
   vocabulary is closed, and it should be fixed before the first component is written. Related: if
   scripting is ever exposed, its numeric types must be the game's fixed-point, not doubles.
2. **Collision.** Keep swept per-pixel time-of-impact, or move to shapes? I recommend keeping the
   *semantics* and rewriting only the expression â it is the most distinctive and most easily-lost
   mechanic, and the AI's lookahead (`cyborg.c:259`) depends on it. If it moves, the melee baseline
   must be re-established from scratch rather than against the current build.
3. **Is the AI in scope for improvement, or is bug-for-bug behaviour part of "same game"?** Its
   target selection depends on `disp_q` traversal order. Settle before the first tournament run.
4. **Resolution.** `ScreenWidth`/`ScreenHeight` are runtime globals feeding `LOG_SPACE_WIDTH` and
   `WRAP_X`/`WRAP_Y` â the battlefield's *topology*, not just spawn placement. Decouple the sim's
   coordinate space in M1 or hard-code 320Ã240 and assert. Not retrofittable.
5. **`optMeleeScale`.** Two camera and LOD paths, currently a user option. "Same feel" means
   picking one is a design decision. Needed before M1.
6. **Voice / 3dovoice.** 110 MB, and it replaces six races' `.txt` outright â so it is not an
   audio-only cut. Under name binding a replacement pack becomes a versioned content contract:
   decide hard-fail vs fall-back-to-base before the format ships.
7. **Ship the melee as a standalone product at M2?** Strong forcing function for polish and gets
   outside eyes on the feel question six months early; creates a support surface.
8. **Who triages the unreachable-phrase set?** It has to happen once, by someone who knows the
   game, or check (2) degrades into a number nobody reads.