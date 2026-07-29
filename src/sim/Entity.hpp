// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_ENTITY_HPP
#define UQM2_SIM_ENTITY_HPP

#include "engine/core/Geometry.hpp"
#include "engine/core/Types.hpp"
#include "sim/Trig.hpp"

#include <entt/entity/entity.hpp>

#include <type_traits>

// Marks ECS components (registry-attached types only). Must stay after all
// includes and be #undef'd at file end -- entt uses `comp` as an identifier
// internally in its own headers.
#define comp

namespace uqm::sim {

// The entity handle: entt's versioned integer id. A stale handle compares
// unequal and reads as dead, never as the next tenant.
using EntityId = entt::entity;

// entt::null never matches a live entity.
inline constexpr EntityId kNoEntity = entt::null;

// Where an element is, and which way it faces. `current` is this frame's
// published position; `next` is where the step is taking it, published at
// Commit (Battle.cpp). A beam carries no Position -- see Beam below.
comp struct Position
{
	Vec2i current;
	Vec2i next;
	Facing facing;
};

// A beam's two endpoints (the PD laser, Human.cpp): geometry, not motion, so
// it never carries Position -- Integrate/Commit's <Position, ...> views never
// see one. Gets Order only; never solid, never moving.
comp struct Beam
{
	Vec2i from;
	Vec2i to;
};

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

// Traversal order as data: a declared, sortable key on every entity.
// Ascending (layer, seq); layers are contiguous segments in enum order,
// FIFO by seq within a layer (Battle::eachOrdered builds the walk).
comp struct Order
{
	Layer layer = Layer::Field;
	u64 seq = 0;
};

// Created this frame: not yet integrated, exempt from its own collisions.
comp struct Appearing
{};

// Skips collisions with another IgnoreSimilar entity sharing its owner.
comp struct IgnoreSimilar
{};

// A countdown to zero, then the death-mark pass attaches Doomed and the reap
// destroys it. Absent means persistent.
comp struct Lifetime
{
	i32 remaining = 0;
};

// Marked for the reap at this frame's sync point; its onDeath, if it had one,
// has already run.
comp struct Doomed
{};

// Immune to weapon damage, with no Lifetime to age or reap: the planet
// (Field.cpp's spawnPlanet, Damage.cpp's weaponCollision).
comp struct Indestructible
{};

// The battlefield's one gravity well (Field.cpp's spawnPlanet).
comp struct Planet
{};

// A single point of a ship's exhaust (tactrans.c:756-790).
comp struct Trail
{
	// Length of the C's colour table, in frames (tactrans.c:757-770).
	static constexpr i32 kLife = 12;
};

// A fading ship-shaped silhouette shed while warping in (tactrans.c:893-930).
comp struct Shadow
{};

// One spark of a dying ship, thrown off in a swarm while the hull is still
// there (tactrans.c:542-615).
comp struct Debris
{
	// How long one spark lasts (tactrans.c:569-571).
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
// (ships/Ilwrath.cpp).
comp struct Cloaked
{};

class Battle;

// `remaining` if `id` has a Lifetime, else the persistent value it implies.
// One rule, so no reader can quietly disagree with another about what an
// absent Lifetime means.
[[nodiscard]] i32 lifeSpanOf(const Battle &b, EntityId id) noexcept;

// Whether `id` holds a Lifetime, i.e. is transient and counting down.
[[nodiscard]] bool isFiniteLife(const Battle &b, EntityId id) noexcept;

}  // namespace uqm::sim

#undef comp

#endif  // UQM2_SIM_ENTITY_HPP
