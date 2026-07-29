// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_ENTITY_HPP
#define UQM2_SIM_ENTITY_HPP

#include "engine/core/Geometry.hpp"
#include "engine/core/Types.hpp"
#include "sim/Trig.hpp"

#include <entt/entity/entity.hpp>

#include <type_traits>

// annotation: marks ECS components -- an entt::registry-attached type, and
// nothing else (not Element's own rump, not a spec, not CollisionMask).
// Empty on purpose: entt uses `comp` as a variable name internally in its
// own headers, so the define stays after every include this file needs and
// is undone at file end, or it would erase that identifier wherever this
// file's includes still had it to expand.
#define comp

namespace uqm::sim {

// The entity handle: entt's versioned integer id -- the old
// EntityId{index, generation} packed into one word, with the same promise:
// a stale handle compares unequal and reads as dead, never as the next
// tenant (review-004 §2).
using EntityId = entt::entity;

// The no-entity value. entt::null never matches a live entity, like the
// old generation-0 handle.
inline constexpr EntityId kNoEntity = entt::null;

// Where an element is, and which way it faces: split out of Element
// (review-007 W4a) since every mover and every reader of a hotspot wants
// this without the rest of Element's body. `current` is this frame's
// published position; `next` is where the step is taking it -- published
// at Commit (Battle.cpp). A beam has no Position at all -- see Beam below.
comp struct Position
{
	Vec2i current;
	Vec2i next;
	Facing facing;
};

// A beam's two ends (the PD laser, Human.cpp): geometry, not motion, so it
// gets its own component instead of abusing Position's current/next the way
// BeamGeometry-tagged Elements used to (review-007 W4a). A beam carries no
// Position at all -- Integrate and Commit's views over <Position, ...>
// never see one, so no exemption is needed. It still gets Order (walked and
// drawn like anything else), but nothing else: never solid, never moving,
// so Motion/Physique/CollisionScratch/Appearing are all dead weight
// (review-007 W4b's minimal-composition rule) -- the collide pass gates on
// collidable(testId) before it ever fetches the rest, so it never meets the
// hole a beam leaves instead of reading through it.
comp struct Beam
{
	Vec2i from;
	Vec2i to;
};

// The declared traversal order (review-005 Y2): what the C encoded by
// insertion position -- head for draw-behind decorations, tail for
// ordnance that acts after its firer -- every spawn now names. Traversal
// visits layers in enum order, stable FIFO within a layer. A mechanic
// needing a new position in the frame (the Pkunk phoenix, which must
// preprocess before the dying ship's death hook) declares a new layer
// here instead of computing an insertion point.
enum class Layer : u8
{
	// Exhaust, warp shadows, debris sparks: drawn behind everything,
	// walked first -- the C's head inserts.
	Background = 0,

	// Ships, asteroids, the planet, in setup order.
	Field = 1,

	// Weapons, beams, blasts, rubble: after the field, so a shot fired
	// this frame is walked into by the catch-up pass -- the C's tail
	// PutElement.
	Ordnance = 2,
};

// The C's disp_q, as data (review-006 Z6): the registry stores, this
// orders -- traversal order is gameplay (sim-architecture.md), so it is a
// declared, sortable key on every entity rather than a walked structure.
// Ascending (layer, seq) is the whole comparator: layers are contiguous
// segments in enum order, FIFO by seq within a layer (Battle::eachOrdered
// builds the sorted walk this describes; the OrderLink spine that used to
// walk it link by link is gone).
comp struct Order
{
	Layer layer = Layer::Field;
	u64 seq = 0;
};

// Created this frame: not yet integrated, exempt from its own collisions.
comp struct Appearing
{
};

// Skips collisions with another IgnoreSimilar entity sharing its owner.
comp struct IgnoreSimilar
{
};

// FiniteLife + lifeSpan, unbundled (review-007 W3): a countdown to zero,
// then the death-mark pass attaches Doomed and the reap destroys it.
// Absent means persistent -- the old NORMAL_LIFE=1 that a Disappearing
// check never decremented.
comp struct Lifetime
{
	i32 remaining = 0;
};

// Marked for the reap at this frame's sync point; its onDeath, if it had
// one, has already run. Disappearing, unbundled from ElementFlags.
comp struct Doomed
{
};

// Immune to weapon damage, no Lifetime to age or reap: the planet
// (Field.cpp's spawnPlanet, Damage.cpp's weaponCollision). misc.c:55's
// lifeSpan = NORMAL_LIFE+1 encoded this as a magic countdown instead.
comp struct Indestructible
{
};

// The battlefield's one gravity well (Field.cpp's spawnPlanet): gravity.c
// keyed on `mass + 1 > GRAVITY_MASS`, a scan that had to read every
// Physique to find the single entity that answers yes.
comp struct Planet
{
};

// A single point of a ship's exhaust (tactrans.c:756-790's cycle_ion_trail).
comp struct Trail
{
	// How long the exhaust fade runs, in frames -- the length of the C's
	// colour table (tactrans.c:757-770).
	static constexpr i32 kLife = 12;
};

// A fading ship-shaped silhouette shed while warping in (tactrans.c:893-930's
// STAMPFILL_PRIM) -- the trail's colour ramp, not its geometry.
comp struct Shadow
{
};

// One spark of a dying ship: the explosion is a swarm thrown off over 26
// frames while the hull is still there (tactrans.c:542-615).
comp struct Debris
{
	// How long one spark of the explosion lasts (tactrans.c:569-571).
	static constexpr i32 kLife = 9;
};

// A weapon's impact flash.
comp struct Blast
{
	// A constant, not derived from sprite frames -- see design-notes V9.
	static constexpr i32 kLife = 5;
};

static_assert(std::is_empty_v<Trail> && std::is_empty_v<Debris>
		&& std::is_empty_v<Blast>);

// Invisible to the eye and to targeting: maintained by the cloak machine
// (ships/Ilwrath.cpp) so no reader has to know what "full cloak level" means.
comp struct Cloaked
{
};

class Battle;

// The value every lifeSpan reader used to see off Element::lifeSpan:
// `remaining` if this entity has a Lifetime, else the literal persistent
// value it replaced (NORMAL_LIFE = 1). One rule, so a reader -- game logic
// or the replay digest -- can never quietly disagree with another about
// what an absent Lifetime means.
[[nodiscard]] i32 lifeSpanOf(const Battle &b, EntityId id) noexcept;

// Whether `id` holds a Lifetime, i.e. is transient and counting down.
// Equivalent to has<Lifetime> now that `ages` is gone; kept as a named
// predicate since call sites read as "is this finite-lived", not "does
// this entity happen to carry a component".
[[nodiscard]] bool isFiniteLife(const Battle &b, EntityId id) noexcept;

}  // namespace uqm::sim

#undef comp

#endif  // UQM2_SIM_ENTITY_HPP
