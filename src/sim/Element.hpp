// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_ELEMENT_HPP
#define UQM2_SIM_ELEMENT_HPP

#include "engine/core/Geometry.hpp"
#include "sim/Collision.hpp"
#include "sim/EntityList.hpp"
#include "sim/Velocity.hpp"

#include <cstdint>

namespace uqm::sim {

class Battle;

// What an element is doing this frame. The C keeps these in one
// ELEMENT_FLAGS word (element.h); the ones the step loop itself reasons about
// are here, and the rest belong to whoever owns the element.
enum class ElementFlags : std::uint32_t
{
	None = 0,

	// Created this frame. Its `next` has not been integrated yet, so the
	// step has to seed it from `current` rather than moving it.
	Appearing = 1u << 0,

	// Reaped at the end of this frame.
	Disappearing = 1u << 1,

	// Counts down `lifeSpan` and disappears at zero.
	FiniteLife = 1u << 2,

	// Takes part in no collisions at all.
	NonSolid = 1u << 3,

	// Does not collide with others sharing its owner -- a flame stream, so
	// it does not eat itself (ilwrath.c:203).
	IgnoreSimilar = 1u << 4,

	// Already collided this frame; the step will not test it again.
	Collided = 1u << 5,

	// Already preprocessed this frame. This is the flag that distinguishes
	// elements present at the start of the frame from ones spawned during
	// it, and it is load-bearing -- see Step.hpp.
	PreProcessed = 1u << 6,

	// Already postprocessed this frame.
	PostProcessed = 1u << 7,

	// Skips position integration for one frame: something else is placing it.
	DefyPhysics = 1u << 8,

	// A player's ship, as opposed to a projectile or a rock.
	PlayerShip = 1u << 9,
};

[[nodiscard]] constexpr ElementFlags
operator|(ElementFlags a, ElementFlags b) noexcept
{
	return static_cast<ElementFlags>(
			static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr ElementFlags
operator&(ElementFlags a, ElementFlags b) noexcept
{
	return static_cast<ElementFlags>(
			static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr ElementFlags
operator~(ElementFlags a) noexcept
{
	return static_cast<ElementFlags>(~static_cast<std::uint32_t>(a));
}
constexpr ElementFlags &
operator|=(ElementFlags &a, ElementFlags b) noexcept
{
	return a = a | b;
}
constexpr ElementFlags &
operator&=(ElementFlags &a, ElementFlags b) noexcept
{
	return a = a & b;
}
[[nodiscard]] constexpr bool
any(ElementFlags f) noexcept
{
	return static_cast<std::uint32_t>(f) != 0;
}

// What kind of thing this is.
//
// The plan's engine primitive #3: a real tag, not a frame pointer. The C
// identifies element types by comparing FRAME pointers (cyborg.c:1222-1227)
// and by reaching into another ship's header to name its constants --
// shofixti.c:251-253 does `#include "../orz/orz.h"` to recognise an Orz
// turret. Both stop being expressible once the type is a field.
enum class ElementKind : std::uint8_t
{
	Unknown = 0,
	Ship,
	Weapon,
	Asteroid,
	Planet,
	Blast,
	Turret,
};

// Hooks. Free functions taking the battle and the element's own id, so they
// have no captured state and cannot outlive it. The C stores these as
// per-instance function pointers and ships mutate their own -- chmmr.c:773
// deletes its hook and pkunk.c:282 reinstalls one -- which the plan
// deliberately gives up in favour of explicit state machines.
using ElementHook = void (*)(Battle &, EntityId) noexcept;

struct Element
{
	// Where it is now, and where the step is taking it.
	Vec2i current;
	Vec2i next;

	Velocity velocity;

	ElementFlags flags = ElementFlags::None;
	ElementKind kind = ElementKind::Unknown;

	// -1 for things nobody owns, like asteroids.
	std::int32_t playerNr = -1;

	std::int32_t facing = 0;
	std::int32_t lifeSpan = 0;
	std::int32_t hitPoints = 0;
	std::int32_t mass = 0;
	std::int32_t damage = 0;

	// Frames until the ship may turn or thrust again. A collision adds to
	// both, which is the stagger you feel after hitting something
	// (collide.c:113-116).
	std::int32_t turnWait = 0;
	std::int32_t thrustWait = 0;

	// Not owned: masks live with the content and outlive the battle.
	const CollisionMask *mask = nullptr;

	ElementHook preProcess = nullptr;
	ElementHook postProcess = nullptr;
	ElementHook onDeath = nullptr;

	// What this element hit, valid only inside a collision hook.
	EntityId collidedWith;

	[[nodiscard]] bool
	collidable() const noexcept
	{
		return mask != nullptr && !any(flags & ElementFlags::NonSolid);
	}
};

}  // namespace uqm::sim

#endif  // UQM2_SIM_ELEMENT_HPP
