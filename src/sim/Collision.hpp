// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_COLLISION_HPP
#define UQM2_SIM_COLLISION_HPP

#include "engine/core/Borrowed.hpp"
#include "engine/core/Geometry.hpp"
#include "engine/core/Types.hpp"

#include <span>
#include <vector>

namespace uqm::sim {

// Swept per-pixel time-of-impact collision: intersec.c, rewritten (per-pixel
// test canvas.c:2047-2056). AI lookahead depends on the semantics
// (cyborg.c:259). Masks, not canvases, headless-safe -- design-notes D2.

using TimeValue = u16;

// A frame is divided into 256 sub-steps, and the walk runs t in 1..257.
inline constexpr int kTimeShift = 8;
inline constexpr TimeValue kMaxTimeValue = (1 << kTimeShift) + 1;  // 257

// A frame's 1-bit opacity plus the hotspot that positions it -- the hotspot
// is part of the collision contract, since a body's position is its hotspot.
// Move-only: masks are per-frame, there are thousands (rule 6).
class CollisionMask
{
public:
	CollisionMask() = default;

	CollisionMask(const CollisionMask &) = delete;
	CollisionMask &operator=(const CollisionMask &) = delete;
	CollisionMask(CollisionMask &&) noexcept = default;
	CollisionMask &operator=(CollisionMask &&) noexcept = default;

	// `opaque` is row-major, one byte per pixel, non-zero where the pixel would
	// collide: a byte, not bool, because that's the source data's shape and
	// std::vector<bool> can't form a span.
	CollisionMask(Extent2u size, Vec2i hotspot, std::span<const u8> opaque);

	[[nodiscard]] Extent2u size() const noexcept { return size_; }
	[[nodiscard]] Vec2i hotspot() const noexcept { return hotspot_; }
	[[nodiscard]] bool empty() const noexcept { return size_.empty(); }

	[[nodiscard]] bool
	opaqueAt(i32 x, i32 y) const noexcept
	{
		if (x < 0 || y < 0 || x >= static_cast<i32>(size_.w)
				|| y >= static_cast<i32>(size_.h))
			return false;
		const usize bit =
				static_cast<usize>(y) * size_.w + static_cast<usize>(x);
		return (words_[bit >> 6] >> (bit & 63)) & 1u;
	}

private:
	std::vector<u64> words_;
	Extent2u size_;
	Vec2i hotspot_;
};

// A body moving from `from` to `to` over one frame. Positions are hotspot
// positions, in the same space the mask's hotspot is measured in.
struct Body
{
	Borrowed<const CollisionMask> mask = nullptr;
	Vec2i from;
	Vec2i to;
};

// Where and when two bodies first touch.
struct Impact
{
	// 0 means no impact. Otherwise the sub-frame time the C returns, which
	// callers compare and order by -- earliest impact wins.
	TimeValue time = 0;

	// Hotspot positions one sub-step before the overlap, matching the C's
	// EndPoint (intersec.c:218-229): bodies land touching, not interpenetrating.
	// Exception: already overlapping at time 1 returns the start positions.
	Vec2i at0;
	Vec2i at1;

	[[nodiscard]] constexpr explicit operator bool() const noexcept
	{
		return time != 0;
	}
};

// The first time in (0, maxTime] at which the two masks overlap by at least
// one pixel, or a falsy Impact.
[[nodiscard]] Impact sweptIntersect(
		const Body &b0, const Body &b1, TimeValue maxTime = kMaxTimeValue);

// Per-entity collision bookkeeping for one frame: already collided (skip
// re-testing this frame) and defying physics (met with no relative motion to
// exchange). Collision-domain, not Battle-private -- Impulse.cpp and
// Damage.cpp write these directly.
struct CollisionScratch
{
	bool collided = false;
	bool defyPhysics = false;
};

// Solidity IS having one (review-007 W2): CollidingElement (collide.h:31-33)
// used to be NONSOLID-and-mask on Element; now it is Collider's presence,
// checked alongside Doomed (Battle::collidable). Attach/detach is the
// runtime toggle where a flag used to flip.
struct Collider
{
	Borrowed<const CollisionMask> mask = nullptr;
};

// A mask value carried independently of Collider, for wherever code needs
// one to survive a stretch of deliberate non-solidity -- attaching a real
// Collider instead would make the holder collide, which is exactly what
// these moments must not do:
//   - the asteroid field's recycle (Field.cpp): the live asteroid keeps a
//     standing copy of its own birth mask, since a kill (doDamage) detaches
//     its Collider on the spot, well before asteroidDeath runs and needs the
//     value for the rubble it spawns; the rubble then carries the same
//     value the same way through its own non-solid life, for rubbleDeath to
//     hand to the replacement asteroid.
//   - a ship warping in (ShipSystems.cpp): intangible on the way in, so
//     applyFacingMask's normal rebuild-from-facingMasks can't be trusted
//     alone -- a spec with no facingMasks (a headless test or replay ship,
//     given its mask directly at spawn instead) has nothing to rebuild from,
//     so arrival falls back to the value stashed here.
struct StashedMask
{
	Borrowed<const CollisionMask> mask = nullptr;
};

}  // namespace uqm::sim

#endif  // UQM2_SIM_COLLISION_HPP
