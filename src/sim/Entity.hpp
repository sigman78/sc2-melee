// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_ENTITY_HPP
#define UQM2_SIM_ENTITY_HPP

#include "engine/core/Types.hpp"

#include <entt/entity/entity.hpp>

namespace uqm::sim {

// The entity handle: entt's versioned integer id -- the old
// EntityId{index, generation} packed into one word, with the same promise:
// a stale handle compares unequal and reads as dead, never as the next
// tenant (review-004 §2).
using EntityId = entt::entity;

// The no-entity value. entt::null never matches a live entity, like the
// old generation-0 handle.
inline constexpr EntityId kNoEntity = entt::null;

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
struct Order
{
	Layer layer = Layer::Field;
	u64 seq = 0;
};

// Created this frame: not yet integrated, exempt from its own collisions.
struct Appearing
{
};

// Skips collisions with another IgnoreSimilar entity sharing its owner.
struct IgnoreSimilar
{
};

// FiniteLife + lifeSpan, unbundled (review-007 W3): a countdown to zero,
// then the death-mark pass attaches Doomed and the reap destroys it.
// Absent means persistent -- the old NORMAL_LIFE=1 that a Disappearing
// check never decremented.
//
// `ages` exists for exactly one entity: the planet (misc.c:55's
// lifeSpan = NORMAL_LIFE+1 = 2, WITHOUT FiniteLife). It needs a real
// countdown *value* -- weaponCollision's target-survives test reads it --
// but must never itself count down. Every other holder has ages = true.
struct Lifetime
{
	i32 remaining = 0;
	bool ages = true;
};

// Marked for the reap at this frame's sync point; its onDeath, if it had
// one, has already run. Disappearing, unbundled from ElementFlags.
struct Doomed
{
};

class Battle;

// The value every lifeSpan reader used to see off Element::lifeSpan:
// `remaining` if this entity has a Lifetime, else the literal persistent
// value it replaced (NORMAL_LIFE = 1). One rule, so a reader -- game logic
// or the replay digest -- can never quietly disagree with another about
// what an absent Lifetime means.
[[nodiscard]] i32 lifeSpanOf(const Battle &b, EntityId id) noexcept;

// Whether `id` is FiniteLife, in the old sense: has a Lifetime that
// actually counts down. The planet's Lifetime{2, false} answers no here
// even though it has one, which is the entire reason this predicate
// exists instead of a plain has<Lifetime> at every call site.
[[nodiscard]] bool isFiniteLife(const Battle &b, EntityId id) noexcept;

}  // namespace uqm::sim

#endif  // UQM2_SIM_ENTITY_HPP
