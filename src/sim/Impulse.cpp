// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Impulse.hpp"

#include "sim/Trig.hpp"
#include "sim/World.hpp"

namespace uqm::sim {

std::uint32_t
isqrt(std::uint32_t value) noexcept
{
	if (value == 0)
		return 0;

	// Bit-by-bit restoring, which is floor(sqrt(v)) exactly and uses no
	// floating point.
	std::uint32_t rem = value;
	std::uint32_t root = 0;
	std::uint32_t bit = std::uint32_t{1} << 30;
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

SpeedState
deriveSpeedState(const Velocity &v, const ThrustProfile &profile) noexcept
{
	const Vec2i c = v.current();
	const std::int64_t speedSq = std::int64_t{c.x} * c.x + std::int64_t{c.y} * c.y;
	const std::int64_t maxVel = worldToVelocity(profile.maxThrust);
	const std::int64_t maxSq = maxVel * maxVel;

	if (speedSq > maxSq)
		return SpeedState::BeyondMax;
	if (speedSq == maxSq)
		return SpeedState::AtMax;
	return SpeedState::Normal;
}

void
applyImpulse(Element &a, Element &b) noexcept
{
	// Impact axis: the line between the two at the moment they met.
	const Vec2i rel{a.next.x - b.next.x, a.next.y - b.next.y};
	int impactA = arctan(rel.x, rel.y);
	int impactB = normalizeAngle(impactA + kHalfCircle);

	const Vec2i va = a.velocity.current();
	const Vec2i vb = b.velocity.current();
	const int travelA = a.velocity.travelAngle();
	const int travelB = b.velocity.travelAngle();

	const Vec2i vrel{va.x - vb.x, va.y - vb.y};
	const int relTravel = arctan(vrel.x, vrel.y);
	const auto speed = static_cast<std::int32_t>(isqrt(static_cast<std::uint32_t>(
			std::int64_t{vrel.x} * vrel.x + std::int64_t{vrel.y} * vrel.y)));

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

	if (a.next == a.current && b.next == b.current)
	{
		// Neither moved, so there is no relative motion to exchange.
		if (any(a.flags & ElementFlags::DefyPhysics)
				&& any(b.flags & ElementFlags::DefyPhysics))
		{
			// Already stuck together. Zero both and skew the impact axis by
			// an octant, which is what eventually works them apart.
			impactA = travelA + (kHalfCircle - kOctant);
			impactB = travelB + (kHalfCircle - kOctant);
			a.velocity.zero();
			b.velocity.zero();
		}
		a.flags |= ElementFlags::DefyPhysics | ElementFlags::Collided;
		b.flags |= ElementFlags::DefyPhysics | ElementFlags::Collided;
	}

	const std::int32_t massA = a.mass;
	const std::int32_t massB = b.mass;
	const std::int64_t scalar =
			std::int64_t{sine(directness, speed << 1)} * (massA * massB);

	const auto push = [&](Element &self, Element &other, int impactAngle,
							  std::int32_t selfMass, std::int32_t otherMass) {
		if (isGravityMass(self.mass + 1))
			return;  // a planet is not pushed

		if (any(self.flags & ElementFlags::PlayerShip))
		{
			// The impulse invalidates the at-max/beyond-max bookkeeping:
			// collide.c:104-110 clears both flags, unconditionally, before
			// the DEFY_PHYSICS test. Without this a bounced ship keeps a
			// stale AtMax and inertial thrust's early-out refuses to
			// accelerate it along its own facing.
			self.ship.speed = SpeedState::Normal;

			// The turn/thrust stagger, gated on DEFY_PHYSICS
			// (collide.c:111-116).
			if (!any(self.flags & ElementFlags::DefyPhysics))
			{
				if (self.turnWait < kCollisionTurnWait)
					self.turnWait += kCollisionTurnWait;
				if (self.thrustWait < kCollisionThrustWait)
					self.thrustWait += kCollisionThrustWait;
			}
		}

		const std::int64_t denom =
				std::int64_t{selfMass} * (selfMass + otherMass);
		if (denom == 0)
			return;
		const auto impulse = static_cast<std::int32_t>(scalar / denom);

		self.velocity.deltaComponents(
				cosine(impactAngle, impulse), sine(impactAngle, impulse));

		// A collision must not leave something almost stationary -- it would
		// sit inside the other shape and collide again. Give it a minimum
		// nudge along the impact axis (collide.c:126-136).
		const Vec2i now = self.velocity.current();
		const std::int32_t mag =
				(now.x < 0 ? -now.x : now.x) + (now.y < 0 ? -now.y : now.y);
		if (velocityToWorld(mag) < kScaledOne)
		{
			const std::int32_t least = worldToVelocity(kScaledOne) - 1;
			self.velocity.setComponents(
					cosine(impactAngle, least), sine(impactAngle, least));
		}
		(void)other;
	};

	push(a, b, impactA, massA, massB);
	push(b, a, impactB, massB, massA);
}

}  // namespace uqm::sim
