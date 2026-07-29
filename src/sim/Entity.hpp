// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_ENTITY_HPP
#define UQM2_SIM_ENTITY_HPP

#include "engine/core/Geometry.hpp"
#include "engine/core/Types.hpp"
#include "sim/Trig.hpp"

#include <entt/entity/entity.hpp>

#include <type_traits>

namespace uqm::sim {

// The entity handle: entt's versioned integer id. A stale handle compares
// unequal and reads as dead, never as the next tenant.
using EntityId = entt::entity;

// entt::null never matches a live entity.
inline constexpr EntityId kNoEntity = entt::null;

// The declared traversal order: named per spawn instead of computed from an
// insertion point. Traversal visits layers in enum order, stable FIFO within
// a layer; a mechanic needing a new position in the frame declares a layer.
enum class Layer : u8
{
	// Drawn behind everything, walked first.
	Background = 0,

	// Ships, asteroids, the planet, in setup order.
	Field = 1,

	// Weapons, beams, blasts, rubble: walked after the field.
	Ordnance = 2,
};

// Every registry-attached type lives here and nothing else does, so `comp::`
// at a use site is the whole answer to "is this a component". An empty one is
// a tag: presence is the value.
//
// The groups are `inline namespace`s, so `comp::life::Doomed` and
// `comp::Doomed` both name it and the everyday spelling is the short one.
// They are documentation, not addressing -- a component changing group must
// not be a tree sweep (review-010 §2).
namespace comp::inline space {

// Where an element is, and which way it faces. `current` is this frame's
// published position; `next` is where the step is taking it, published at
// Commit (Battle.cpp). A beam carries no Position -- see Beam below.
struct Position
{
	Vec2i current;
	Vec2i next;
	Facing facing;
};

// A beam's two endpoints (the PD laser, Human.cpp): geometry, not motion, so
// it never carries Position -- Integrate/Commit's <Position, ...> views never
// see one. Gets Order only; never solid, never moving.
struct Beam
{
	Vec2i from;
	Vec2i to;
};

// Traversal order as data: a declared, sortable key on every entity.
// Ascending (layer, seq); layers are contiguous segments in enum order,
// FIFO by seq within a layer (Battle::ordered builds the walk).
struct Order
{
	Layer layer = Layer::Field;
	u64 seq = 0;
};

}  // namespace comp::inline space

namespace comp::inline matter {

// The battlefield's one gravity well (Field.cpp's spawnPlanet).
struct Planet
{};

}  // namespace comp::inline matter

namespace comp::inline life {

// Created this frame: not yet integrated, exempt from its own collisions.
struct Appearing
{};

// A countdown to zero, then the death-mark pass attaches Doomed and the reap
// destroys it. Absent means persistent.
struct Lifetime
{
	i32 remaining = 0;
};

// Marked for the reap at this frame's sync point; its onDeath, if it had one,
// has already run.
struct Doomed
{};

// Immune to weapon damage, with no Lifetime to age or reap: the planet
// (Field.cpp's spawnPlanet, Damage.cpp's weaponCollision).
struct Indestructible
{};

}  // namespace comp::inline life

namespace comp::inline owner {

// Skips collisions with another IgnoreSimilar entity sharing its owner.
struct IgnoreSimilar
{};

}  // namespace comp::inline owner

namespace comp::inline ship {

// Invisible to the eye and to targeting: maintained by the cloak machine
// (ships/Ilwrath.cpp).
struct Cloaked
{};

}  // namespace comp::inline ship

namespace comp::inline look {

// A single point of a ship's exhaust (tactrans.c:756-790).
struct Trail
{
	// Length of the C's colour table, in frames (tactrans.c:757-770).
	static constexpr i32 kLife = 12;
};

// A fading ship-shaped silhouette shed while warping in (tactrans.c:893-930).
struct Shadow
{};

// One spark of a dying ship, thrown off in a swarm while the hull is still
// there (tactrans.c:542-615).
struct Debris
{
	// How long one spark lasts (tactrans.c:569-571).
	static constexpr i32 kLife = 9;
};

// A weapon's impact flash.
struct Blast
{
	// A constant, not derived from sprite frames -- see design-notes V9.
	static constexpr i32 kLife = 5;
};

static_assert(std::is_empty_v<Trail> && std::is_empty_v<Debris>
		&& std::is_empty_v<Blast>);

}  // namespace comp::inline look

class Battle;

// Whether `id` is transient: counting down to a death of its own. An absent
// Lifetime means persistent, and is the only encoding of it.
[[nodiscard]] bool isTransient(const Battle &b, EntityId id) noexcept;

// Frames of life left. Asserts a Lifetime rather than standing in a value
// for its absence -- the C's NORMAL_LIFE stood in as 1, which reads as "one
// frame left" and hid two dead branches until it was removed (review-010).
// Ask isTransient first where absence is possible.
[[nodiscard]] i32 framesLeft(const Battle &b, EntityId id) noexcept;

// Frames lived, given the span it was spawned with: the counter runs down
// and everything that animates off it counts up.
[[nodiscard]] i32 ageOf(const Battle &b, EntityId id, i32 span) noexcept;

}  // namespace uqm::sim

#endif  // UQM2_SIM_ENTITY_HPP
