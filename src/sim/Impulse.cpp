// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Impulse.hpp"

#include "engine/core/Types.hpp"
#include "sim/Trig.hpp"
#include "sim/World.hpp"

namespace uqm::sim {

u32 isqrt(u32 value) noexcept
{
	if (value == 0)
		return 0;

	// Bit-by-bit restoring, which is floor(sqrt(v)) exactly and uses no
	// floating point.
	u32 rem = value;
	u32 root = 0;
	u32 bit = u32{1} << 30;
	while (bit > rem)
		bit >>= 2;

	while (bit != 0)
	{
		if (rem >= root + bit)
		{
			rem -= root + bit;
			root = (root >> 1) + bit;
		}
		else
		{
			root >>= 1;
		}
		bit >>= 2;
	}
	return root;
}

SpeedState deriveSpeedState(
		const Velocity &v, const ThrustProfile &profile) noexcept
{
	const Vec2i c = v.current();
	const i64 speedSq = i64{c.x} * c.x + i64{c.y} * c.y;
	const i64 maxVel = worldToVelocity(profile.max);
	const i64 maxSq = maxVel * maxVel;

	if (speedSq > maxSq)
		return SpeedState::BeyondMax;
	if (speedSq == maxSq)
		return SpeedState::AtMax;
	return SpeedState::Normal;
}

void applyImpulse(const Position &aPos, Motion &aMotion, const Physique &aPhys,
		ShipState *aShip, CollisionScratch &aScratch, const Position &bPos,
		Motion &bMotion, const Physique &bPhys, ShipState *bShip,
		CollisionScratch &bScratch) noexcept
{
	// Impact axis: the line between the two at the moment they met.
	const Vec2i rel{aPos.next.x - bPos.next.x, aPos.next.y - bPos.next.y};
	int impactA = arctan(rel.x, rel.y);
	int impactB = normalizeAngle(impactA + kHalfCircle);

	const Vec2i va = aMotion.velocity.current();
	const Vec2i vb = bMotion.velocity.current();
	const int travelA = aMotion.velocity.travelAngle();
	const int travelB = bMotion.velocity.travelAngle();

	const Vec2i vrel{va.x - vb.x, va.y - vb.y};
	const int relTravel = arctan(vrel.x, vrel.y);
	const auto speed = static_cast<i32>(isqrt(
			static_cast<u32>(i64{vrel.x} * vrel.x + i64{vrel.y} * vrel.y)));

	int directness = normalizeAngle(relTravel - impactA);
	if (directness <= kQuadrant || directness >= kHalfCircle + kQuadrant)
	{
		// They only scraped. Promote it to head-on, or they scrape again next
		// frame and the frame after -- two ships would grind along each other
		// indefinitely (collide.c:54-62).
		directness = kHalfCircle;
		impactA = travelA + kHalfCircle;
		impactB = travelB + kHalfCircle;
	}

	if (aPos.next == aPos.current && bPos.next == bPos.current)
	{
		// Neither moved, so there is no relative motion to exchange.
		if (aScratch.defyPhysics && bScratch.defyPhysics)
		{
			// Already stuck together. Zero both and skew the impact axis by
			// an octant, which is what eventually works them apart.
			impactA = travelA + (kHalfCircle - kOctant);
			impactB = travelB + (kHalfCircle - kOctant);
			aMotion.velocity.zero();
			bMotion.velocity.zero();
		}
		aScratch.defyPhysics = true;
		aScratch.collided = true;
		bScratch.defyPhysics = true;
		bScratch.collided = true;
	}

	const i32 massA = aPhys.mass;
	const i32 massB = bPhys.mass;
	const i64 scalar = i64{sine(directness, speed << 1)} * (i64{massA} * massB);

	const auto push = [&](Motion &selfMotion, ShipState *self,
							  CollisionScratch &selfScratch, int impactAngle,
							  i32 selfMass, i32 otherMass) {
		if (isGravityMass(selfMass + 1))
			return;  // a planet is not pushed

		if (self != nullptr)
		{
			// The turn/thrust stagger, gated on DEFY_PHYSICS
			// (collide.c:111-116).
			if (!selfScratch.defyPhysics)
			{
				if (self->turnWait < kCollisionTurnWait)
					self->turnWait += kCollisionTurnWait;
				if (self->thrustWait < kCollisionThrustWait)
					self->thrustWait += kCollisionThrustWait;
			}
		}

		const i64 denom = i64{selfMass} * (selfMass + otherMass);
		if (denom == 0)
			return;
		const auto impulse = static_cast<i32>(scalar / denom);

		selfMotion.velocity.deltaComponents(
				cosine(impactAngle, impulse), sine(impactAngle, impulse));

		// A collision must not leave something almost stationary -- it would
		// sit inside the other shape and collide again. Give it a minimum
		// nudge along the impact axis (collide.c:126-136).
		const Vec2i now = selfMotion.velocity.current();
		const i32 mag =
				(now.x < 0 ? -now.x : now.x) + (now.y < 0 ? -now.y : now.y);
		if (velocityToWorld(mag) < kScaledOne)
		{
			const i32 least = worldToVelocity(kScaledOne) - 1;
			selfMotion.velocity.setComponents(
					cosine(impactAngle, least), sine(impactAngle, least));
		}
	};

	push(aMotion, aShip, aScratch, impactA, massA, massB);
	push(bMotion, bShip, bScratch, impactB, massB, massA);
}

}  // namespace uqm::sim
