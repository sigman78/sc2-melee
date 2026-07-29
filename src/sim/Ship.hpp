// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_SHIP_HPP
#define UQM2_SIM_SHIP_HPP

#include "engine/core/Borrowed.hpp"
#include "engine/core/Types.hpp"
#include "sim/Damage.hpp"
#include "sim/Element.hpp"
#include "sim/Spawn.hpp"
#include "sim/Thrust.hpp"

#include <optional>
#include <type_traits>

namespace uqm::sim {

class Battle;

// Free functions taking the battle and the element's id: no captured state,
// so they can't outlive it. The C mutates per-instance hooks at runtime
// (chmmr.c:773, pkunk.c:282); this doesn't.
using ShipPhase = void (*)(Battle &, EntityId) noexcept;

// What the player is asking for this frame.
enum class ShipInput : u8
{
	None = 0,
	Left = 1u << 0,
	Right = 1u << 1,
	Thrust = 1u << 2,
	Weapon = 1u << 3,
	Special = 1u << 4,
};

[[nodiscard]] constexpr ShipInput operator|(ShipInput a, ShipInput b) noexcept
{
	return static_cast<ShipInput>(static_cast<u8>(a) | static_cast<u8>(b));
}
[[nodiscard]] constexpr ShipInput operator&(ShipInput a, ShipInput b) noexcept
{
	return static_cast<ShipInput>(static_cast<u8>(a) & static_cast<u8>(b));
}
constexpr ShipInput &operator|=(ShipInput &a, ShipInput b) noexcept
{
	return a = a | b;
}
[[nodiscard]] constexpr bool any(ShipInput f) noexcept
{
	return static_cast<u8>(f) != 0;
}

namespace comp::inline shot {

// Guidance parameters copied from the spec at fire time, plus the shot's
// own tracking clock. Attached by the fire block when the spec declares
// guidance; guidedShotPreProcess is its system function.
struct Guided
{
	i32 trackWait = 0;
	i32 maxSpeed = 0;
	i32 thrustScale = 0;
	i32 clock = 0;
};

}  // namespace comp::inline shot

// A ship's weapon: the primary-fire descriptor plus the shot's own flight
// parameters, guidance, and per-frame/collision hooks.
struct WeaponSpec
{
	i32 wait = 0;
	i32 energyCost = 0;

	// Geometry, handed to the spawn function below (Spawn.hpp's ShipView):
	// needs the ship's own facing to resolve into a Position/Motion, so it
	// stays scalar rather than an attach-ready component literal.
	i32 speed = 0;
	i32 muzzleOffset = 0;

	// The fire block attaches these verbatim to a shot. `lifetime.remaining`
	// and `vitality.hitPoints` are also read as plain scalars elsewhere
	// (guidedShotPreProcess, ShipView) -- the spec itself never decrements.
	comp::Lifetime lifetime;
	comp::Vitality vitality;
	comp::Warhead warhead;

	// Guided-weapon parameters (human.c:36-44); absent for a weapon that
	// just flies straight, which is most of them.
	std::optional<comp::Guided> guided;

	// The primary weapon, as a pure descriptor function (Spawn.hpp).
	SpawnFn spawn = nullptr;

	// Stamped onto every shot as FrameDriven: true only for the flame, whose
	// AnimFrame and Collider mask follow every frame it lives
	// (ilwrath.c:126-139). A guided shot's frame follows its facing instead.
	bool frameDriven = false;

	// A shot collides as whichever sprite frame it is drawn from, which is
	// not always its facing -- indexing by facing instead is what squashes a
	// missile into the wrong box.
	std::span<const CollisionMask> masks;
};

// A ship's SPECIAL: cost, cooldown and what it does.
struct SpecialSpec
{
	i32 wait = 0;
	i32 energyCost = 0;

	// What SPECIAL does in the post phase; the engine only ticks the
	// counter, everything a special does is per-ship (ship.c:342-346). Null
	// when the special lives in the ship's preProcess hook instead (Avenger).
	ShipPhase hook = nullptr;

	// LASER_RANGE (human.c:55), in display pixels. Zero for a ship without
	// point defence.
	i32 pointDefenceRange = 0;
};

// A ship's energy plant: what it holds and how it refills.
struct Battery
{
	i32 max = 0;
	i32 regen = 0;
	i32 wait = 0;
};

// An immutable description of a ship type: a *value*, constructible in code,
// not only parsed -- sis_ship.c:881-990 builds the flagship's descriptor from
// inventory, shofixti.c:461-517 a damaged variant by copying one.
struct ShipSpec
{
	i32 maxCrew = 0;

	ThrustProfile thrust;
	Battery battery;
	i32 thrustWait = 0;
	i32 turnWait = 0;

	i32 mass = 0;

	WeaponSpec weapon;
	SpecialSpec special;

	// The ship's own per-frame hook, run inside shipPreProcess after energy
	// regen and before turning -- RACE_DESC.preprocess_func (ship.c:232-236).
	// The Ilwrath cloak engages here, winning the energy race against the shot.
	ShipPhase preProcess = nullptr;

	Borrowed<const CollisionMask> hullMask = nullptr;

	// One mask per facing, in facing order; empty until content is loaded,
	// since sim/ never reads files. A ship's silhouette follows its facing,
	// so collision has to track it rather than whichever cel spawned it.
	std::span<const CollisionMask> facingMasks;

	// Every field defaults to zero, which is not a slow ship but one that
	// cannot accelerate at all, turns every frame and has no crew -- worth
	// being able to tell from a control bug.
	[[nodiscard]] constexpr bool valid() const noexcept
	{
		return maxCrew > 0 && thrust.max > 0;
	}
};

namespace comp::inline ship {

// A ship's mutable half. The C keeps this in STARSHIP beside the ELEMENT;
// here it is a registry component keyed by the same entity, destroyed
// with it.
struct ShipState
{
	// Hooks hold a ShipState& across spawns and other hooks (shipPostProcess'
	// weapon loop); stable addresses are load-bearing, exactly as for Element.
	static constexpr auto in_place_delete = true;

	Borrowed<const ShipSpec> spec = nullptr;

	i32 crew = 0;
	i32 energy = 0;

	i32 energyCounter = 0;
	i32 weaponCounter = 0;
	i32 specialCounter = 0;

	SpeedState speed = SpeedState::Normal;

	// SHIP_IN_GRAVITY_WELL (races.h:71): orthogonal to the at-max/beyond-max
	// pair. Lets a ship accelerate past its own max, up to kMaxAllowedSpeed
	// (ship.c:82,106-112); gravity sets it, next thrust clears it
	// (ship.c:263-267).
	bool inGravityWell = false;

	// Frames until the ship may turn or thrust again. A collision adds to
	// both, which is the stagger you feel after hitting something
	// (collide.c:113-116, Impulse.cpp).
	i32 turnWait = 0;
	i32 thrustWait = 0;
};

// What the player is asking for, as its own component: every ship has one
// (Battle::attachShip), so the app writes it without a ShipState field.
struct Input
{
	ShipInput buttons = ShipInput::None;
};

}  // namespace comp::inline ship

namespace comp::inline shot {

// Which WeaponSpec a shot came from: read for collision masks and cel
// lookup as much as for steering. The pointer is copied out by readers,
// never held into the pool, so default storage suffices.
struct FromWeapon
{
	Borrowed<const WeaponSpec> spec = nullptr;
};

}  // namespace comp::inline shot

namespace comp::inline ship {

// Only a ship with one carries it. `level`: 0 = solid (STAMP), 1..5 =
// STAMPFILL fade steps, kFullLevel (6) = fully cloaked (black). Walked
// one step per frame, reversed to uncloak (ilwrath.c:255-273).
struct Cloak
{
	// Five visible fill colours (levels 1..5), then black.
	static constexpr i32 kVisibleColours = 5;
	static constexpr i32 kFullLevel = kVisibleColours + 1;

	i32 level = 0;
};

// Presence is the phase; removal replaces the C's per-instance hook swap
// (tactrans.c:868-886, 703-728). shipPreProcess dispatches on these.
struct WarpingIn
{
	// HYPERJUMP_LIFE (element.h:69): how long a ship spends warping in,
	// invisible and untouchable, before it becomes real.
	static constexpr i32 kFrames = 15;

	// TRANSITION_SPEED (tactrans.c:909): how far apart the images of an
	// arriving ship are spaced along its path.
	static constexpr i32 kImageSpacing = displayToWorld(40);
};

struct Exploding
{
	// NUM_EXPLOSION_FRAMES (element.h:71).
	static constexpr i32 kFrames = 12;

	// NUM_EXPLOSION_FRAMES * 3 (element.h:71, tactrans.c:714).
	static constexpr i32 kLife = kFrames * 3;

	// When the hull itself stops being drawn: 15 frames into the 36-frame
	// death (tactrans.c:569-571).
	static constexpr i32 kHullVanishAge = 15;
};

}  // namespace comp::inline ship

static_assert(
		std::is_empty_v<comp::WarpingIn> && std::is_empty_v<comp::Exploding>);

namespace comp::inline life {

// cleanup_dead_ship (tactrans.c:307-337): attached the instant a ship
// starts exploding (startShipExplosion). The death path (Battle.cpp)
// checks this tag instead of a per-element function pointer.
struct SweepsOwnedOnDeath
{};

}  // namespace comp::inline life

}  // namespace uqm::sim

#endif  // UQM2_SIM_SHIP_HPP
