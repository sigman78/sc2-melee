// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Velocity.hpp"

namespace uqm::sim {

void
Velocity::setAxis(std::int32_t v, std::int32_t &vector, std::int32_t &incr,
		std::int32_t &fract) noexcept
{
	if (v >= 0)
	{
		vector = velocityToWorld(v);
		incr = makeWord(1, 0);
	}
	else
	{
		v = -v;
		vector = -velocityToWorld(v);
		// The doubled remainder in the high byte is what makes
		// `fract - hi(incr)` reconstruct a negative component.
		incr = makeWord(0xFF, remainder(v) << 1);
	}
	fract = remainder(v);
}

void
Velocity::setComponents(std::int32_t dx, std::int32_t dy) noexcept
{
	const int angle = arctan(dx, dy);
	if (angle == kFullCircle)
	{
		// ARCTAN's zero-vector sentinel. The C zeroes everything *except*
		// the travel angle, which it then sets to the sentinel below -- so a
		// stopped object reports "no direction" rather than "up".
		const int keep = angle;
		*this = Velocity{};
		travelAngle_ = keep;
		return;
	}

	setAxis(dx, vector_.x, incr_.x, fract_.x);
	setAxis(dy, vector_.y, incr_.y, fract_.y);
	error_ = Vec2i{};
	travelAngle_ = angle;
}

void
Velocity::setVector(std::int32_t magnitude, Facing facing) noexcept
{
	const int angle = facing.angle().raw();
	travelAngle_ = angle;

	magnitude = worldToVelocity(magnitude);
	const std::int32_t dx = cosine(angle, magnitude);
	const std::int32_t dy = sine(angle, magnitude);

	setAxis(dx, vector_.x, incr_.x, fract_.x);
	setAxis(dy, vector_.y, incr_.y, fract_.y);
	error_ = Vec2i{};
	// Note: unlike setComponents, the C does *not* recompute the angle from
	// the components here -- the facing is authoritative, so a magnitude of
	// zero still remembers which way the object was pointed.
}

void
Velocity::deltaComponents(std::int32_t dx, std::int32_t dy) noexcept
{
	const Vec2i now = current();
	setComponents(dx + now.x, dy + now.y);
}

Vec2i
Velocity::advance(int frames) noexcept
{
	Vec2i out;

	// `e` is a COUNT (uint16) in the C, and the remainder is masked back into
	// the error each time, so the accumulator cannot run away.
	const std::int32_t ex = error_.x + fract_.x * frames;
	out.x = vector_.x * frames + loSigned(incr_.x) * (ex >> kVelocityShift);
	error_.x = remainder(ex);

	const std::int32_t ey = error_.y + fract_.y * frames;
	out.y = vector_.y * frames + loSigned(incr_.y) * (ey >> kVelocityShift);
	error_.y = remainder(ey);

	return out;
}

}  // namespace uqm::sim
