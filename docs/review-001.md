# Review 001 — `src/` soundness and cross-reference against the C

Scope: the whole `src/` tree as of `rewrite/core` (melee vertical slice, ~10,200
lines), reviewed for soundness and architecture, then cross-referenced against
`sc2/src/uqm` with physics, collision, weapons and ship specials as the
priority. Every claim below was verified against the actual C source, not from
memory; C citations name the file and line in `sc2/src`.

Verdict up front: **the architecture is sound and the physics core is
faithful.** `inertial_thrust`, the packed velocity encoding, the trig tables,
`TFB_Random`, the impulse formula, gravity, and the world constants all match
the C exactly. The real divergences cluster in two places: the **step loop**
(process.c's live-list walk became snapshot passes, which is the root cause of
about six findings) and the **Ilwrath cloak** (written from a misreading of
ilwrath.c, five findings). A third theme: several comments citing the C are
wrong, which matters in a tree that uses citations as its spec.

Fix status is tracked in the log at the end.

---

## A. Findings, ranked by gameplay impact

IDs are stable so commits can reference them.

### A1. Weapons have `mass = 0`, so weapon-vs-weapon collisions never happen — REAL BUG

`testPair` faithfully reproduces `CollisionPossible`'s "at least one side has
mass" guard (collide.h:38 → Battle.cpp), but in the C a missile's
`mass_points` *is* its damage (weapon.c:101) and a laser's is 1 (weapon.c:58).
`shipPostProcess` sets `w.mass = 0`. Consequence: the Avenger's flame cannot
burn down incoming nukes and nukes pass through each other — and
flame-intercepts-nuke is the central interaction of the M1 matchup.

### A2. Energy regen never pauses while firing — REAL BUG

The C's `DeltaEnergy` sets `energy_counter = energy_wait` on **every
successful call, including weapon and special spends** (status.c:317-323).
The C++ `deltaEnergy` never touches the counter; only the regen branch arms
it. A continuously-firing C Avenger (cost 1, WEAPON_WAIT 0, regen 4@4) burns
16 energy in 16 frames and then sputters at ~50% duty; the C++ one regains +4
every 5 frames *while* firing — ~80% duty and a far longer opening burst.
Every energy-costed weapon fires materially faster in the C++. Related minor:
the C re-arms the counter only on a *successful* DeltaEnergy, so a failed
negative-regen drain retries every frame (affects no current ship).

### A3. Weapons are inert one frame longer than the C — REAL BUG (step loop)

In the C, `PostProcessQueue` walks the *live* list, so a missile appended by
`ship_postprocess` is caught up by the same walk (process.c:842-862): it is
PreProcessed (moves; `initialize_missile`'s one-frame velocity back-off at
weapon.c:126-127 makes the first move end at the muzzle) and PostProcessed
(APPEARING cleared) on the fire frame, and can collide from frame N+1.
`Battle::postProcessPass` ran its catch-up *before* the commit loop that
calls the hooks, over a snapshot — so a weapon fired at frame N did not move
until N+1 and, because `Appearing` also survived a frame, could not collide
until N+2. Total travel was muzzle+60·v vs the C's muzzle+59·v. The comments
at Ship.cpp ("the step loop's catch-up pass picks it up") and Battle.cpp
claimed otherwise. Side effect: the melee app's fire-sound detection scanned
for `Appearing` after `step()` and only worked *because* of this bug.

### A4. The cloak is wrong in five compounding ways — REAL BUGS

Against ilwrath.c:

- **Untargetable during the entire fade.** `OBJECT_CLOAKED` in the C is
  STAMPFILL **and BLACK** (element.h:201-204) — invisible to TrackShip
  (weapon.c:343-346) and PD (human.c:203-204) only when fully black, i.e. 5
  frames after activation, and targetable again on the first decloak step.
  The C++ set `Cloaked` whenever `cloakLevel > 0`: nuke-proof from the
  activation frame and while fading back in after firing. A significant
  unmarked buff. (Sub-item: the ion trail was suppressed during the whole
  ramp; the C suppresses only when black, ship.c:271 — and the C++ comment's
  claim that the C "cannot produce one either" is false: `spawn_ion_trail`
  builds an independent POINT_PRIM, tactrans.c:792-832.)
- **There is no voluntary decloak in the C.** A second press with the counter
  at 0 re-cloaks: pays SPECIAL_ENERGY_COST again, resets the fill to WHITE,
  counter = 13 (ilwrath.c:377-393). The C++ invented a free toggle-off. The
  cited ilwrath.c:251-253 is only the ramp-step condition.
- **Firing (the only real decloak) zeroes `special_counter`**
  (ilwrath.c:347), so re-cloak is available the very next frame. The C++
  armed a 13-frame lockout on its toggle-off and left the counter running on
  fire.
- **Re-cloak mid-fade restarts from white** (activation always resets the
  fill, ilwrath.c:381-383); the C++ resumed from the current level.
- **The decloak auto-aim is missing.** Firing from full black TrackShips the
  *ship*, leads the target by LOOK_AHEAD = 4 frames of both velocities, and
  snaps ShipFacing to the predicted bearing (ilwrath.c:281-342). This is the
  Avenger's signature ambush snap.

Plus two structural halves:

- **Wrong phase.** The cloak engages in the C's *preprocess* (via
  `preprocess_func`, ship.c:232-236), before that frame's turn/thrust/weapon;
  the C++ ran all specials at the end of postprocess. With energy 3, C cloaks
  and the shot fails; C++ shoots and the cloak fails.
- **Ramp one colour short in the renderer.** The C walks 5 visible colours
  then black (ilwrath.c:349-371); the melee renderer hid the ship at level
  `kCloakSteps-1`, so only 4 colours ever drew.

### A5. Every special recycles one frame slow — REAL BUG

The C decrements `special_counter` and *then* runs the ship hook, which tests
`== 0` the same frame (ship.c:342-346; human.c:340-343) — PD fires every 9
frames, cloak re-arms in exactly 13. The C++ ran the special only in the
`else` branch of the counter test: periods 10 and 14.

### A6. `applyImpulse` does not reset the ship's speed state — REAL BUG

collide.c:104-110 (and 141-147) clears `SHIP_AT_MAX_SPEED |
SHIP_BEYOND_MAX_SPEED` on any player ship taking an impulse — ungated by
DEFY_PHYSICS. The C++ `push()` never touched `ship.speed`, so a bounced ship
could keep a stale `AtMax` and `thrust()`'s early-out would refuse to
accelerate along its facing. Impulse.hpp's NOTE *claimed* the reset was kept.

### A7. Planet collisions cost 1 crew instead of crew/4 — REAL BUG

In the C, a player ship's `hit_points` **is** `crew_level` — one union field
(element.h:126-133) — so `collision()`'s `hit_points >> 2` (ship.c:364-367)
costs an 18-crew Cruiser 4 crew and a full Avenger 5. The C++ split the
fields; a ship's `hitPoints` stays 0 and the floor made every planet impact
cost exactly 1. The Damage.cpp comment reasoned from the split fields and
missed the union. **Porting rule going forward: every C `hit_points` read on
a PLAYER_SHIP is a crew read.**

### A8. Collision impact fields are one sub-step late — LATENT

`frame_intersect` returns the last *clear* position (corners update only
after a failed test, intersec.c:218-229); `sweptIntersect` filled
`Impact::at0/at1` from the live (overlapping) corners. Latent today because
`testPair` rewinds from `hit.time` alone and nothing reads `at0/at1` — but
the fields and the Collision.hpp comment ("what the C writes back through
EndPoint") were wrong, and the first future consumer would inherit the bug.

### A9. Seam handling inverted, and worse than the C — REAL BUG

The C never wraps during PreProcess (`next += delta`, process.c:163-175);
positions wrap only at the display commit (process.c:899-916). A
seam-crossing element therefore carries an out-of-range `next` through
collision testing — seam collisions are *missed* for one frame. The C++
wrapped `next` during integration, so a seam crossing became a sweep from
x≈8191 to x≈1 — `sweptIntersect` walks it as a full-arena traversal and can
manufacture **phantom hits** against anything near the path. Different
failure mode; the C++ one is worse.

### A10. The "BAD NEWS" overlap protocol was reduced to a skip — OPEN

process.c:397-505, for two already-overlapping non-finite elements, also:
(i) retests against a stationary partner when the scanner already has
COLLISION; (ii) kills an APPEARING element with unchanged frames outright
(do_damage for its full hit_points — an asteroid spawned inside the planet
dies on the spot); (iii) if a frame *changed*, reverts `next.image` to
`current`, re-inits the intersect controls and ShipFacing, and retries — you
cannot rotate into a wall in the C. The C++ only skips the pair (the skip
condition itself — time 1, both sides non-finite — is faithful).

### A11. Earliest-collision-wins and post-impulse rescan dropped — OPEN

process.c:531-540: before resolving a pair at time t, the C recursively asks
whether either party hits anything *earlier* (min_time = t−1) and abandons
the current pair for the frame if so. process.c:603-606: after `collide()`,
both participants immediately re-scan the whole list, so pile-ups chain
within one frame. The C++ resolves strictly in list order, once; three-body
collisions resolve a frame late and in-frame damage ordering differs. (This
is also the only place `CollisionPossible`'s both-COLLISION skip ever bites,
so its absence in `testPair` is moot until this is ported.)

### A12. The flame's hitbox is wrong twice — REAL BUG

The C's flame animates every frame (`flame_preprocess`, ilwrath.c:126-139,
turn_wait/next_turn 0) and CHANGING re-inits the intersect frame
(process.c:159-160), so its collision silhouette *grows* through the 8 fire
frames of its 8-frame life. The C++ gave the Avenger no `weaponPreProcess`
and indexed `weaponMasks` by **facing** — but the flame's masks are animation
frames, not facings, so it collided all life as whichever cel `facing % 8`
selected. (For the nuke the facing indexing is correct — 16 facing cels.)

### A13. Asteroids never spin backwards, and the RNG stream is short — REAL BUG

misc.c draws **seven** values per asteroid: edge selector (156), position
(163/168), speed `(&7)+4` (179-180), heading (181), sprite facing (188),
`turn_wait = thrust_wait = rand & 3` (189-191), and `thrust_wait |= rand &
(1<<7)` (192-193) — the spin *direction*. Field.cpp drew six and never set
bit 7: every asteroid spins forward and the stream desyncs by one draw per
asteroid. Field.hpp's "exactly six RNG draws" claim was wrong about the C.

### A14. Nuke tracks 3 frames early — REAL BUG (minor)

`initialize_nuke` seeds `turn_wait = TRACK_WAIT` (human.c:297-299): first
steer on the 4th hook frame. The C++ spawn left `turnWait` 0, so the nuke
tracked on its first frame; same cadence after, phase 3 frames early.

### A15. Point defence ignores the Cruiser's own missiles — DELIBERATE, revisit

The C's target filter is only `!= self && CollidingElement && !CLOAKED`
(human.c:203-204) — no ownership test — and the nuke's flags are 0, so the C
Cruiser pays for and shoots down its own in-flight nukes within range. That
is a real tactical constraint (you cannot hold SPECIAL with a nuke out). The
C++ skipped same-player elements. Skipping *gravity masses* is a separate,
documented and verified-accurate deliberate change (the C fires at the
planet, which absorbs the shot: energy wasted, do_damage exempts it,
misc.c:214).

### A16. Weapon stop/pierce protocol hardcoded — OPEN (post-M1)

The C moves an element to the impact point only when its own collision_func
newly raised COLLISION (process.c:572-596); `weapon_collision` raises it
*conditionally* (weapon.c:141-164): a weapon with `hit_points > target
mass_points` hitting a surviving finite target does not stop. The C++
hardcoded "finite-life stops and dies". No consequence for nuke/flame (hp 1);
matters for pierce-through munitions and FINITE_LIFE quasi-ships (Chmmr
zapsats, Ur-Quan fighters).

### A17. Dead ship's ordnance is not swept — REAL BUG (minor)

`cleanup_dead_ship` (tactrans.c:307-337): when the explosion finishes, every
element still owned by the dead ship is deleted (except drifting crew). In
the C++ a dead Cruiser's nuke flew out its full 60 frames.

### A18. Explosion spark drift quantized and RNG-sliced differently — REAL BUG (minor)

C drift: `SetVelocityComponents(COSINE(angle, d), SINE(angle, d))` with a
byte angle → 64 directions; speed `HIBYTE(LOWORD(rand)) % 5` (one byte).
C++: `setVector(speed, angleToFacing(drift))` → 16 directions, and the speed
slice used 24 bits. Same schedule and placement otherwise (verified
bit-for-bit); replay parity and visual spread differ.

### A19. Smaller confirmed items

- **Spent nuke lingers one frame.** C sets DISAPPEARING (same-frame removal,
  weapon.c:175-177); the C++ left it drawn one extra frame — which happens to
  match the C *flame*, whose `flame_collision` wrapper clears DISAPPEARING
  (ilwrath.c:141-148). The two weapons need different linger behaviour.
- **Hook nudges to `next` are lost**: C integrates `next += delta`
  (process.c:172-173, and crew_preprocess depends on it); the C++ computed
  `next = current + delta`.
- **Disappearing elements got a posthumous postprocess + commit**; the C
  removes them without either (process.c:873-879).
- **Blast lifetime** is a constant 5; the C derives 9 (nuke, from saturn.ani
  frame count) and 2 (flame) (weapon.c:215-236). Visual only; documented.
- **Born-exemption shape**: process.c:389-394 puts FINITE_LIFE on *either*
  side and APPEARING+life>1 on *either* side; the C++ required them on the
  same element. Diverging case: the planet (APPEARING, non-finite, life 2) vs
  a weapon on the spawn frame.
- **`trackShip` return convention**: C returns −1 no-target / 0 dead-ahead
  (weapon.c:412); the C++ returned 0 for both. Harmless for the nuke (`> 0`
  never tested), load-bearing for the cloak auto-aim (`>= 0`).
- **TrackShip stickiness**: the C re-evaluates a locked `hTarget` only
  (weapon.c:328-335); the C++ rescans nearest every call. Identical in 1v1;
  diverges with >2 ships.
- **PD range test wraps the torus** (C++ improvement, uncommented); the C is
  blind across the seam (human.c:208-217).
- **PD applies damage directly** instead of spawning a colliding laser
  element (C: hit_points 1, mass 1, LASER_LIFE 1, weapon.c:44-85; the beam
  can be intercepted en route and leaves a blast). Documented deliberate;
  edge cases differ.
- **Intersec seek arithmetic** is int32 where the C truncates through a
  16-bit COUNT (intersec.c:116,141) — diverges only at sweeps ≥ ~256
  display px/frame.
- **Ion-trail offset**: C uses hotspot-to-bottom-edge of the ship frame
  (tactrans.c:809-811); C++ uses mask height/2 — equal only for centred
  hotspots.
- **F1 debug toggle fires on level, not edge** — held F1 flickers the overlay
  at 24 Hz (main.cpp; `consume()` returns held|pressed).
- **Fixed battle seed** `0x2A5B` in melee main: every battle identical.
- **`g.lastShots` is dead** and `(void)beams` papered over an unused count.

## B. Wrong comments and citations (spec hazards)

The tree treats C citations as its spec; each of these would mislead later
porting work.

| Where | Claim | Reality |
| --- | --- | --- |
| Element.hpp (gravity-mass note) | "collide.c asks it without [the +1] too, so it still takes an impulse" | collide.c:102/139 both use `mass_points + 1`; a fleeing ship at mass 100 takes **no** impulse in the C. The Impulse.cpp code is faithful; the comment was wrong. |
| Impulse.hpp NOTE | "applyImpulse still *resets* the state the way collide.c:109-110 does" | The code never touched `ship.speed` (A6). |
| Ship.cpp / Battle.cpp | fired weapon "is caught up by the step loop… can hit on the frame it was fired" | Not for post-pass spawns before the A3 fix. |
| Field.hpp | "Call it [spawnPlanet] before the asteroids, as init.c does" | init.c:228-233 spawns the 5 **asteroids first**, then the planet. Following the comment desyncs the stream. |
| Field.cpp (rubble) | head insertion "because spawn_rubble calls PutElement before filling the element in (misc.c:90)" | `PutElement` is `PutQueue` — appends at the **tail** (displist.c:142-165). The pkunk head-insert citation in Battle.hpp *is* correct. |
| Field.hpp | asteroid spawn "consumes exactly six RNG draws" | Seven (A13). |
| Field.cpp (DISPLAY_ALIGN) | "the truncation is why an asteroid never appears in the far reaches" | 16-bit % 8192 covers the full width; the gloss is confused. Truncation is reproduced correctly regardless. |
| Damage.cpp (weaponCollision) | "The weapon is spent either way (weapon.c:179-181)" | Those lines sit *inside* weapon.c:161-164's conditional (A16). |
| Damage.cpp (solidCollision) | "it has never produced anything else [than 1]" | hit_points is crew via the union (A7). |
| Collision.hpp (Impact) | at0/at1 are "what the C writes back through EndPoint" | The C writes the position one sub-step *before* overlap (A8). |
| Element.hpp / Ship.cpp (cloak) | "Press again, or fire, and it drops (ilwrath.c:249-253, 346-347)" | No voluntary decloak exists; re-press re-cloaks and pays (A4). |
| tests/sim_test.cpp (cruiserView) | "MISSILE_SPEED == MAX_THRUST (human.c:31, 44)" = 24 | `MISSILE_SPEED = max(MAX_THRUST, DISPLAY_TO_WORLD(10))` = **40**. Ship.cpp is right. |
| Ship.hpp (Impulse plan note) | druuge.c:266 | druuge.c:262 (cosmetic). |

## C. Architecture assessment

### Keep as-is

- **EntityList**: arena + ordered intrusive list + generational handles +
  epoch-checked `EntityRef` is exactly right for `disp_q`'s observable
  ordering; stable addresses match the C's fixed-pool guarantee that every
  ship in `ships/` assumes.
- **Pure spawn descriptors** (`SpawnFn`: const view in, values out) kill the
  Umgah heap-churn defect class by construction.
- **`CollisionEvent` as observational data** is the right pattern for
  presentation consumers; extended to spawn events as part of the A3 fix.
- **Pacer** (fixed-step accumulator with bounded catch-up) and
  **InputAccumulator** (held + sticky pressed) are correct and tested.
- **Layering is real**: nothing in `sim/` does I/O, wall-clock, threads or
  atomics (verified by inspection); headless tests never link SDL.
- Content/platform layers are clean; `Resources` id-indirection is
  load-bearing and correctly reasoned.

### Structural decisions needed (before/at M2)

1. **The step loop was the one real architectural defect.** Findings A3, A9,
   A19 (posthumous postprocess, next-overwrite) and half of A11's context
   trace to replacing process.c's live-list interleaved walk with snapshot
   passes. The fix is a live walk in both passes (the C's own
   `hNext-after-processing` pattern), not per-symptom patches.
2. **Sound/HUD must be declared outputs of `step()`** (the plan already says
   so). The melee app's flag-scanning depended on the A3 bug. Spawn events
   join `collisions()`; death/fire events will follow as consumers appear.
3. **`ShipData::special` needs a phase.** One postprocess hook cannot express
   the C, where specials hang off preprocess (Ilwrath) or postprocess
   (Cruiser) with different energy-race consequences (A4). Introduced
   `specialInPreProcess` as part of the A4 fix; revisit when the component
   library (plan §Ship model) lands.
4. **Decide the hit_points/crew union policy** (A7). Either mirror the union
   for ships or audit every C `hit_points` site during porting. The audit
   already failed once.
5. **RNG stream discipline needs a test, not comments.** Three findings (A13,
   the Field.hpp spawn-order comment, A18's bit slicing) desync replays.
   Since the plan's verification strategy is replay-based, add a golden
   stream test: fixed seed → spawn field → assert positions/spins, pinned
   once against the C.
6. **Weapon identity is underspecified.** A weapon carries the whole
   `ShipData*` for guidance parameters, its sprite frame was conflated with
   its facing (A12), and stop/pierce policy is hardcoded (A16). The plan's
   `WeaponSpec` split should own: mask policy (by-facing vs by-animation),
   guidance block, linger-on-hit, pierce rule.
7. **Match outcome is provisional by design** (plan §What we accept losing):
   "died but may still win" is not modelled; the melee main decides the
   winner from element removal. Fine for M1; the match loop takes it later.
8. **melee main.cpp (1176 lines)** mixes setup, renderer, HUD and sound
   policy. Acceptable for the slice; the plan's Modes/Transition structure is
   still unstarted and M2 will force it.

### Deliberate divergences on record (fine, keep documented)

Zoom-independent collision masks (SpriteSet.hpp — hitboxes no longer shrink
with the camera); interpolated impact placement instead of
DISPLAY_TO_WORLD-snapped (Battle.cpp, documented); PD skips planets
(Ship.cpp, citation verified); PD works across the seam; input accumulator
(feel change, documented); `minSeparation` at spawn (Field.hpp, documented);
single continuous zoom instead of the optMeleeScale fork (Camera.hpp); no
graphics-context dependency in collision (Collision.hpp — the C's
`ContextActive()` bail and the weapon.c rejection-loop hang are both real,
citations verified). Consider consolidating these into one divergence ledger
file; they are currently scattered across comments.

## D. Verified faithful (do not re-litigate)

- `thrust()` vs `inertial_thrust` (ship.c:55-147): branch-for-branch,
  including the gravity-well early-out, all strict/non-strict comparisons,
  the half-increment-on/full-increment-off turn math, and the
  "don't slow a whipped ship" guard (the C's own ship.c:116-118).
- `Velocity` vs velocity.c: packed incr encoding (lo=±1, hi=doubled
  remainder), reconstruction, Bresenham error carry, setVector keeping the
  facing-derived angle, zero-vector sentinel.
- Trig.hpp vs trans.c: all 64 sine entries, the 33-entry arctan table,
  ARCTAN's rounding and quadrant folds, ANGLE_TO_FACING round-half-up.
- `Rng` vs TFB_Random: bit-exact including the uint32-wrap divergence from
  Park–Miller; seed coercion; compile-time golden vectors.
- `applyImpulse` vs collide.c: scalar formula and truncation order, scrape
  promotion window (`<= QUADRANT || >= HALF+QUADRANT`), DEFY_PHYSICS branch
  (velocity zeroing, octant skew), COLLISION_TURN/THRUST_WAIT guards,
  minimum-nudge — everything except A6.
- gravity.c in its entirety: pull WORLD_TO_VELOCITY(1), threshold 255 display
  px, per-axis-then-squared rejection, self's-PRE_PROCESS endpoint choice,
  AT_MAX-only clear (BEYOND left set), early break inside a source's well.
- World.hpp: 8192×7680 derivation, single-fold WRAP_VAL, WRAP_DELTA `<=`
  boundary ties.
- intersec.c sweep: window derivation (sign flips, ±1 widening, fract
  scaling, the "lesser despite `>`" timeEnd), the Bresenham walk and seek,
  box-then-pixel gating, t0≤1/t0==0 start handling (A8's field fill and the
  16-bit seek edge aside).
- Step-loop flag lifecycle: death at life 0 in the pre pass with death_func
  and flag re-read, the local-flags APPEARING trick for player ships,
  IGNORE_VELOCITY-only motion gate, FINITE_LIFE decrement placement,
  solid-solid-only momentum exchange, DEFY_PHYSICS expiry at commit.
- Every number in both ship tables vs human.c:27-55 / ilwrath.c:27-53,
  including MISSILE_SPEED 40 (the min-speed clamp), MAX_MISSILE_SPEED 80,
  THRUST_SCALE 4, LASER_RANGE 100 display px.
- TrackShip core: filter (PLAYER_SHIP, other player, not cloaked, `life_span
  && crew`), next-vs-current by tracker's PRE_PROCESS for both parties,
  WRAP_DELTA, |dx|+|dy| with strict `<` first-found tiebreak, one facing step
  per call, the exactly-astern coin flip `((rand & 1) << 1) − 1`.
- Nuke ramp: `MISSILE_SPEED + (MISSILE_LIFE − life)·THRUST_SCALE` capped 80,
  SetVelocityVector every frame, tracking gated by turn_wait only.
- Warp-in (15 frames, TRANSITION_SPEED 160, 12-frame shadows marching
  inward), ion trail (12 frames, head-inserted), explosion (36-frame life,
  26-frame spark schedule verified bracket by bracket, 9-frame sparks, hull
  vanish at 15). Death sparks are NEUTRAL with generic explosion frames in
  both.
- weapon_collision damage eligibility `FINITE_LIFE || life_span ==
  NORMAL_LIFE` (planet at life 2 excluded in both); blast offset along the
  travel angle; deltaCrew's strict `>`; doDamage's three cases without
  gravity.c's `+1`.
- Field/misc: planet (hp 200, life 2, placement loop, mass assigned only
  after placement — reasoning verified genuine), asteroid draw order and
  values, rubble (life 5, NONSOLID, death→respawn chain), DISPLAY_ALIGN
  16-bit truncation, TimeSpaceMatterConflict's ship-in-transition exception,
  kNumAsteroids 5.

## E. Omissions (C behaviour with no C++ counterpart yet)

- Ilwrath decloak auto-aim (part of A4).
- `cleanup_dead_ship` ordnance sweep and surviving-crew fleet record
  (tactrans.c:302-337) — sweep is A17; crew record is campaign-side.
- `ship_death` winner determination / battle_counter / ditty
  (tactrans.c:729-749) — deliberately moved to the match loop per the plan.
- LOW_ON_ENERGY flag (status.c:318) — AI input, not yet needed.
- hTarget/Untarget infrastructure — subsumed by per-frame Cloaked checks
  except TrackShip stickiness (A19).
- TrackShip's APPEARING-tracker exemption (weapon.c:343-346) — no M1 caller.
- Per-event ship sounds beyond what the melee app plays; hit-severity sounds
  exist (weapon.c:166-173, ship.c:369-372) and the app approximates them from
  CollisionEvents.
- DeltaCrew's Samatra guard (status.c:339-341) — full game only.

## F. Recommended fix order and log

Order: cheap high-impact sim fixes first, then the step loop (with step
outputs so audio survives it), then the cloak batch rewritten directly from
ilwrath.c, then weapon identity, with comment corrections riding along in
each. A10/A11/A16 (BAD NEWS protocol, earliest-collision-wins + rescan,
pierce rule) are deferred with this document as their record — they need the
collision resolution loop reshaped and should be one piece of work.

| # | Finding | Status |
| --- | --- | --- |
| 1 | A1 weapon mass = damage | **fixed** (Ship.cpp; test testOpposingMissilesDestroyEachOther) |
| 2 | A2 deltaEnergy arms the counter on success | **fixed** (Ship.cpp; test testFiringPostponesEnergyRegen) |
| 3 | A5 special decrement-then-run | **fixed** (Ship.cpp; test testSpecialFiresTheFrameItsCounterExpires) |
| 4 | A6 impulse clears speed state | **fixed** (Impulse.cpp + hpp note) |
| 5 | A7 planet damage from crew | **fixed** (Damage.cpp; planet test now asserts crew/4) |
| 6 | A14 nuke turnWait seed | **fixed** (Ship.cpp) |
| 7 | A13 asteroid 7th draw + spin direction | **fixed** (Field.cpp/hpp) |
| 8 | A3+A9+A19 step loop live walk, unwrapped `next +=` integration, wrap-at-commit, no posthumous postprocess, spawn events, missile back-off (weapon.c:126-127) | **fixed** (Battle.cpp/hpp, Ship.cpp, melee main sound path) |
| 9 | A4 cloak batch: ShipData::preProcess phase, black-only Cloaked, C re-cloak semantics, counter zeroing, auto-aim (ilwrathPreProcess/cloakedAutoAim), 5-colour ramp, trackShip -1 return | **fixed** (Element/Ship/melee renderer; tests updated + testCloakedFiringSnapAims) |
| 10 | A12+A19 weapon frame/mask identity (colorCycle), flame animation + growth, flame linger vs nuke same-frame vanish | **fixed** (Ship.\*, Damage.cpp, melee renderer) |
| 11 | A8 impact fields last-clear + B comment corrections (Element.hpp gravity note, Impulse.hpp, Field.hpp order/draw-count, rubble tail, Damage.cpp citations, Collision.hpp EndPoint, sim_test MISSILE_SPEED, DISPLAY_ALIGN gloss) | **fixed** |
| — | A17 dead-ship ordnance sweep | **fixed** (sweepDeadShipOrdnance on the wreck's death) |
| — | A18 spark drift 64 directions + byte speed slice | **fixed** (Ship.cpp) |
| — | F1 edge-trigger, varying printed battle seed, dead locals | **fixed** (melee main) |
| — | A10, A11, A16 collision-resolution protocol | **fixed** — ProcessCollisions ported whole (Battle.cpp): the BAD NEWS repairs (stationary retest, overlap-spawn execution, rotation revert via Element::priorMask/priorFacing), earliest-collision-wins recursion, post-impulse whole-list rescans, and hook-decided stopping with the weapon pierce rule (Damage.cpp, weapon.c:141-181). The born-exemption shape (A19, either-side) rode along. Tests: pierce, rotation revert, overlap-spawn execution. |
| — | A15 PD own-missiles policy | **fixed** — decided faithful: no ownership filter, the Cruiser burns its own nukes (test testPointDefenceBurnsOwnNuke) |
| — | POST_PROCESS protection | **fixed** — a regression fix 8 introduced and this port caught: the C's POST_PROCESS flag guards committed elements from whole-list catch-up walks (process.c:859); the live-walk commit had cleared it, so a ship firing every frame was integrated twice per frame (test testCommittedElementsAreNotIntegratedTwice) |
| — | A19 TrackShip target stickiness (hTarget) | deferred — no effect until >2 ships |
| — | A19 ion-trail offset (frame bottom edge vs mask height/2) | open, visual only |
| — | A14/B blast lifetimes 9 (nuke) / 2 (flame) vs constant 5 | open, visual only (needs frame counts from content) |

All fixes verified by the full suite: content, sim (69 checks incl. 5 new
regressions), engine — green as of this revision. Fixes 1-7 are each a small,
independently revertable change; fix 8 restructured `Battle::preProcessPass`/
`postProcessPass` into the C's live walks; fix 9 replaced the cloak model
wholesale (ShipState::cloaking is gone, cloakLevel is now the C's prim state
as an index 0..6).
