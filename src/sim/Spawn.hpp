// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_SPAWN_HPP
#define UQM2_SIM_SPAWN_HPP

#include "engine/core/Geometry.hpp"
#include "sim/Velocity.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace uqm::sim {

// Pure spawn descriptors: engine primitive #5, and the one that fixes a live
// defect rather than merely tidying one.
//
// The AI asks "would firing hit anything?" by *actually firing*.
// cyborg.c:339-410 copies the ship element, advances a tick, calls
// init_weapon_func on the copy, runs the intercept test, and frees the
// results. That happens every lookahead frame, for every candidate.
//
// The copy is the ELEMENT. It is not the STARSHIP -- GetElementStarShip hands
// back the shared one -- so any write a weapon-init makes through
// RaceDescPtr survives the speculative call. And they do:
//
//   - umgah.c:330-341, inside initialize_cone, calls SetCustomShipData
//     (an HFree plus an HMalloc) and rewrites ship_data.special[0]. Every AI
//     lookahead frame therefore churns the heap and mutates the ship's
//     sprite, from a call whose results are thrown away.
//   - orz.c:249-253 is the compensating hack for the same shape: it bumps
//     TurretPtr->turn_wait before ship_intelligence and decrements it after,
//     to undo a side effect it knows is coming.
//
// The fix is to make the write impossible rather than discouraged. A spawn
// function takes a ShipView by const reference and fills a caller-owned
// buffer with values. It cannot allocate, cannot touch the ship, and running
// it a hundred times costs the same as running it once -- which is what
// lookahead needs.
//
// Umgah's cached prevFacing disappears in the process. It exists only to
// avoid resetting the cone's animation frame, but the frame is a pure
// function of the facing, so it can simply be computed. The state was there
// to work around the mutation, not to hold anything.

// What a spawn function may read. Deliberately small: if a weapon needs
// something not here, that is a conversation about the interface, not a
// reason to reach for a global.
struct ShipView
{
	Vec2i position;          // hotspot, world units
	Velocity velocity;
	int facing = 0;          // 0..15
	int playerNr = 0;

	// The ship's own weapon parameters, from its data file.
	std::int32_t weaponSpeed = 0;
	std::int32_t weaponLife = 0;
	std::int32_t weaponDamage = 0;
	std::int32_t weaponHitPoints = 0;
	std::int32_t muzzleOffset = 0;   // pixels forward of the hotspot
	std::int32_t blastOffset = 0;
};

// What comes out. A value, not an object: no handle, no allocation, nothing
// to free on the discard path the AI takes.
struct Spawn
{
	Vec2i position;
	int facing = 0;
	std::int32_t speed = 0;
	std::int32_t life = 0;
	std::int32_t damage = 0;
	std::int32_t hitPoints = 0;
	std::int32_t blastOffset = 0;
	int playerNr = 0;

	// Which sprite frame, derived rather than remembered -- this is the field
	// Umgah mutates the ship to set.
	std::uint16_t frameIndex = 0;

	// True where the projectile should not collide with others from the same
	// source (IGNORE_SIMILAR).
	bool ignoreSimilar = false;

	friend bool operator==(const Spawn &, const Spawn &) = default;
};

// The C's Weapon[6] is the ceiling on one shot across every ship.
inline constexpr std::size_t kMaxSpawnsPerShot = 6;
using SpawnBuffer = std::array<Spawn, kMaxSpawnsPerShot>;

// A ship's primary-weapon spawn. Const view in, values out, count returned.
//
// The signature is the guarantee: there is no non-const path to the ship, so
// the Umgah pattern does not compile.
using SpawnFn = std::size_t (*)(const ShipView &, std::span<Spawn>) noexcept;

// Where a projectile appears: `muzzleOffset` pixels along the facing from the
// hotspot. Pulled out because every ship does it and half of them do it
// slightly differently in the C.
[[nodiscard]] Vec2i muzzlePosition(const ShipView &ship) noexcept;

// --------------------------------------------------------------------------
// The two M1 weapons, as pure functions.

// Earthling Cruiser: one forward missile.
[[nodiscard]] std::size_t spawnCruiserPrimary(
		const ShipView &ship, std::span<Spawn> out) noexcept;

// Ilwrath Avenger: one forward flame.
[[nodiscard]] std::size_t spawnAvengerPrimary(
		const ShipView &ship, std::span<Spawn> out) noexcept;

}  // namespace uqm::sim

#endif  // UQM2_SIM_SPAWN_HPP
