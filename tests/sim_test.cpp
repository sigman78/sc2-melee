// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.
//
// sim/ tests. Much of what matters here is asserted at compile time -- the
// RNG's golden vectors are `static_assert`s, so this file merely has to
// compile for them to have been checked. What is left is the behaviour that
// needs a running object: stream independence and reseed semantics.

#include "sim/Battle.hpp"
#include "sim/Collision.hpp"
#include "sim/Damage.hpp"
#include "sim/EntityList.hpp"
#include "sim/Field.hpp"
#include "sim/Gravity.hpp"
#include "sim/Impulse.hpp"
#include "sim/Ship.hpp"
#include "sim/Random.hpp"
#include "sim/Spawn.hpp"
#include "sim/Thrust.hpp"
#include "sim/Trig.hpp"
#include "sim/Velocity.hpp"
#include "sim/World.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace uqm;       // Vec2i
using namespace uqm::sim;  // Rng, EntityList, the world constants

namespace {

int failures = 0;

#define CHECK(cond, ...)                                                      \
	do                                                                        \
	{                                                                         \
		if (!(cond))                                                          \
		{                                                                     \
			std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                  \
			std::printf(__VA_ARGS__);                                         \
			std::printf("\n");                                                \
			++failures;                                                       \
		}                                                                     \
	} while (0)

void
testStreamsAreIndependent()
{
	// The plan makes presentation draw from its own stream so it cannot
	// perturb the simulation (commanim.c currently draws from the sim's, on a
	// wall-clock schedule). That only works if streams are objects, not one
	// global -- so: two Rngs with the same seed agree, and drawing from one
	// does not move the other.
	Rng sim(999);
	Rng presentation(999);
	CHECK(sim.next() == presentation.next(), "same seed, same first draw");

	const std::uint32_t before = sim.seed();
	(void)presentation.next();
	(void)presentation.next();
	CHECK(sim.seed() == before, "drawing from one stream must not move another");
}

void
testReseedReturnsThePrevious()
{
	// TFB_SeedRandom returns the old seed; the C uses that to save and
	// restore a stream around a private one (melnorm.c's getStripRandomSeed).
	Rng rng(4321);
	const std::uint32_t saved = rng.reseed(1000);
	CHECK(saved == 4321, "reseed returns the previous seed, got %u", saved);
	CHECK(rng.seed() == 1000, "reseed installs the new seed");

	const std::uint32_t a = rng.next();
	rng.reseed(1000);
	CHECK(rng.next() == a, "restoring a seed replays the stream");
}

void
testTruncationWidthMatters()
{
	// Call sites truncate to 16 bits before taking a modulo -- `(COUNT)`,
	// `(UWORD)`, `LOWORD` -- and that changes the answer. If these ever
	// agreed, one of the two is wrong.
	Rng wide(12345);
	Rng narrow(12345);
	const std::uint32_t full = wide.next() % 100u;
	const std::uint32_t trunc = narrow.next16() % 100u;
	CHECK(full != trunc,
			"16-bit truncation must change the result for this seed "
			"(%u vs %u)", full, trunc);
}

// --------------------------------------------------------------------------
// Entity list

std::vector<int>
order(const EntityList<int> &list)
{
	std::vector<int> out;
	for (EntityId id = list.front(); id.valid(); id = list.next(id))
		out.push_back(*list.get(id));
	return out;
}

void
testTraversalOrderIsWhatWasAskedFor()
{
	EntityList<int> list;
	const EntityId a = list.pushBack(1);
	const EntityId b = list.pushBack(2);
	list.pushBack(3);
	CHECK(order(list) == std::vector<int>({1, 2, 3}), "pushBack appends");

	// The pkunk.c:498-512 case: head insertion so the phoenix preprocesses
	// before the dead Pkunk's death_func runs.
	list.pushFront(0);
	CHECK(order(list) == std::vector<int>({0, 1, 2, 3}), "pushFront prepends");

	// Splicing into the middle, which 20 InsertElement sites do.
	list.insertAfter(a, 99);
	CHECK(order(list) == std::vector<int>({0, 1, 99, 2, 3}),
			"insertAfter splices");

	list.remove(b);
	CHECK(order(list) == std::vector<int>({0, 1, 99, 3}),
			"removal keeps the rest in order");
}

void
testSlotReuseDoesNotReorder()
{
	// The reason an arena alone will not do: once the free list hands a slot
	// back, slot order and traversal order disagree. A new entity must land
	// where it was asked to, not where its storage happens to be.
	EntityList<int> list;
	list.pushBack(1);
	const EntityId second = list.pushBack(2);
	list.pushBack(3);

	list.remove(second);              // frees the middle slot
	list.pushBack(4);                 // which this reuses
	CHECK(order(list) == std::vector<int>({1, 3, 4}),
			"a reused slot must not drag the entity back to its old position");

	list.pushFront(5);
	CHECK(order(list) == std::vector<int>({5, 1, 3, 4}),
			"and pushFront still prepends after reuse");
}

void
testStaleHandlesAreDetectable()
{
	EntityList<int> list;
	const EntityId id = list.pushBack(7);
	CHECK(list.alive(id) && list.get(id) != nullptr, "a live handle resolves");

	list.remove(id);
	CHECK(!list.alive(id), "a removed handle is not alive");
	CHECK(list.get(id) == nullptr, "and does not resolve");

	// The slot comes straight back; the generation is what stops the stale
	// handle reading its new tenant.
	const EntityId reused = list.pushBack(8);
	CHECK(reused.index == id.index, "the test needs the slot to be reused");
	CHECK(!list.alive(id), "the stale handle must not resolve to the new one");
	CHECK(list.get(id) == nullptr, "still nullptr after reuse");
	CHECK(*list.get(reused) == 8, "the new handle works");

	CHECK(!list.alive(kNoEntity), "a default handle is never alive");
	CHECK(list.get(kNoEntity) == nullptr, "and never resolves");
}

void
testRemovalDuringTraversal()
{
	// Stepping removes entities while walking the list -- a projectile
	// expiring mid-frame -- so the successor has to be read before the
	// removal, and that has to keep working.
	EntityList<int> list;
	for (int i = 0; i < 6; ++i)
		list.pushBack(i);

	EntityId id = list.front();
	while (id.valid())
	{
		const EntityId nextId = list.next(id);
		if (*list.get(id) % 2 == 0)
			list.remove(id);
		id = nextId;
	}
	CHECK(order(list) == std::vector<int>({1, 3, 5}),
			"removing while walking should leave the odd ones in order");
	CHECK(list.size() == 3, "size should track removals, got %zu", list.size());
}

void
testEntityAddressesAreStable()
{
	// Ship code holds a pointer to itself across a spawn -- fire a weapon,
	// then write the cooldown back. If allocating could move entities that
	// write would land in moved-from memory, which is exactly the bug this
	// caught: the cooldown silently never took, and the ship fired every
	// frame. Growth must not relocate anyone.
	EntityList<int> list;
	std::vector<int *> addresses;
	std::vector<EntityId> ids;
	for (int i = 0; i < 500; ++i)
	{
		ids.push_back(list.pushBack(i));
		addresses.push_back(list.get(ids.back()).raw());
	}

	for (std::size_t i = 0; i < ids.size(); ++i)
	{
		CHECK(list.get(ids[i]).raw() == addresses[i],
				"entity %zu moved when the arena grew", i);
		CHECK(*addresses[i] == static_cast<int>(i),
				"entity %zu holds the wrong value after growth", i);
	}

	// And a slot recycled through the free list is still addressable, with a
	// generation that retires the old handle.
	const EntityId old = ids[10];
	list.remove(old);
	const EntityId reused = list.pushBack(-1);
	CHECK(list.get(old) == nullptr, "the stale handle must not resolve");
	CHECK(list.get(reused) != nullptr, "the recycled slot should be live");
}

// --------------------------------------------------------------------------
// World topology

void
testWorldWrapping()
{
	// The arena the C computes today, now fixed at compile time.
	static_assert(kLogSpaceWidth == 8192 && kLogSpaceHeight == 7680);

	static_assert(wrapX(0) == 0);
	static_assert(wrapX(kLogSpaceWidth) == 0, "the far edge is the near edge");
	static_assert(wrapX(-1) == kLogSpaceWidth - 1, "stepping off the left");
	static_assert(wrapY(-1) == kLogSpaceHeight - 1);
	static_assert(wrap(Vec2i{-1, -1})
			== Vec2i{kLogSpaceWidth - 1, kLogSpaceHeight - 1});

	// The shorter way round. Two entities either side of the seam are close,
	// and an AI that measures the long way flies away from its target.
	static_assert(wrapDeltaX(1) == 1);
	static_assert(wrapDeltaX(kLogSpaceWidth - 1) == -1,
			"just short of a full lap is one unit backwards");
	static_assert(wrapDeltaX(-(kLogSpaceWidth - 1)) == 1);
	static_assert(wrapDeltaX(kLogSpaceWidth / 2) == kLogSpaceWidth / 2,
			"exactly half stays positive, as WRAP_DELTA_X does");
	static_assert(wrapDeltaY(kLogSpaceHeight - 1) == -1);

	// Display/world conversion: a pixel is four world units.
	static_assert(displayToWorld(1) == 4);
	static_assert(worldToDisplay(4) == 1);
	static_assert(worldToDisplay(displayToWorld(kSpaceWidth)) == kSpaceWidth);
}

// --------------------------------------------------------------------------
// Trig

void
testTrigRoundTrips()
{
	// The table's golden values are static_asserts in Trig.hpp; what is worth
	// checking at runtime is that the pieces agree with each other over the
	// whole circle.

	// Every angle's (cos, sin) must arctan back to itself. This is the
	// property the AI leans on -- aim at a target, then read the aim back --
	// and it is where a table transcribed one entry out would show up.
	for (int a = 0; a < kFullCircle; ++a)
	{
		const std::int32_t x = cosine(a, 1000);
		const std::int32_t y = sine(a, 1000);
		const int back = arctan(static_cast<int>(x), static_cast<int>(y));
		CHECK(back == a, "angle %d -> (%ld, %ld) -> %d", a,
				static_cast<long>(x), static_cast<long>(y), back);
	}

	// Magnitude is preserved to within rounding: cos^2 + sin^2 == m^2.
	for (int a = 0; a < kFullCircle; ++a)
	{
		const std::int64_t x = cosine(a, 10000);
		const std::int64_t y = sine(a, 10000);
		const std::int64_t r2 = x * x + y * y;
		// 10000^2 == 1e8; allow half a percent for the 14-bit table.
		CHECK(r2 > 99000000 && r2 < 101000000,
				"angle %d has magnitude^2 %lld, expected ~1e8", a,
				static_cast<long long>(r2));
	}

	// Opposite angles nearly negate -- and the "nearly" is the point.
	//
	// SINE is `(table[a] * m) >> 14`, and an arithmetic right shift *floors*
	// rather than truncating toward zero, so a negative product rounds away
	// from zero where the matching positive one rounds towards it. sine(1) is
	// -4077 while sine(33) is +4076. The function is therefore not symmetric
	// under a half turn, by one count, wherever the product is not a multiple
	// of 16384.
	//
	// That asymmetry is in the shipped game, so it is asserted rather than
	// smoothed: a rewrite that rounded symmetrically would drift from the C
	// by a unit per frame in exactly half the directions.
	int asymmetric = 0;
	for (int a = 0; a < kFullCircle; ++a)
	{
		const std::int32_t here = sine(a, 4096);
		const std::int32_t opposite = sine(a + kHalfCircle, 4096);
		const std::int32_t sum = here + opposite;
		CHECK(sum == 0 || sum == -1,
				"sine(%d) and sine(%d) should sum to 0 or -1, got %ld", a,
				a + kHalfCircle, static_cast<long>(sum));
		if (sum != 0)
			++asymmetric;
	}
	CHECK(asymmetric > 0,
			"the floor-shift asymmetry must survive; if this ever reaches 0 "
			"someone has made the rounding symmetric");
}

void
testArctanSentinel()
{
	// The zero vector returns an *unnormalized* 64. A caller that forwards it
	// to a table lookup without checking would be reading a direction it was
	// never given, so the sentinel has to survive rather than fold to 0.
	CHECK(arctan(0, 0) == kFullCircle,
			"the zero vector must return the unnormalized sentinel");
	CHECK(arctan(0, 0) != 0, "and must not look like 'up'");
	CHECK(normalizeAngle(arctan(0, 0)) == 0,
			"normalizing it is the caller's decision, and gives 0");
}

// --------------------------------------------------------------------------
// Collision

// A solid w x h mask with its hotspot at the centre.
CollisionMask
solid(std::uint32_t w, std::uint32_t h)
{
	const std::vector<std::uint8_t> bits(static_cast<std::size_t>(w) * h, 1);
	return CollisionMask(Extent2u{w, h},
			Vec2i{static_cast<std::int32_t>(w / 2),
				static_cast<std::int32_t>(h / 2)},
			bits);
}

// A ring: opaque border, hollow middle. Two of these can overlap by bounding
// box while missing entirely, which is the whole point of per-pixel.
CollisionMask
ring(std::uint32_t w, std::uint32_t h)
{
	std::vector<std::uint8_t> bits(static_cast<std::size_t>(w) * h, 0);
	for (std::uint32_t y = 0; y < h; ++y)
		for (std::uint32_t x = 0; x < w; ++x)
			if (x == 0 || y == 0 || x == w - 1 || y == h - 1)
				bits[static_cast<std::size_t>(y) * w + x] = 1;
	return CollisionMask(Extent2u{w, h},
			Vec2i{static_cast<std::int32_t>(w / 2),
				static_cast<std::int32_t>(h / 2)},
			bits);
}

void
testCollisionNeedsNoGraphicsContext()
{
	// intersec.c:245 returns "no collision" whenever there is no active
	// graphics context, which is why a naive headless build hangs in
	// weapon.c's rejection loop instead of producing wrong numbers. Nothing
	// here has ever seen a renderer.
	const CollisionMask a = solid(4, 4);
	const CollisionMask b = solid(4, 4);
	const Body b0{&a, Vec2i{0, 0}, Vec2i{0, 0}};
	const Body b1{&b, Vec2i{0, 0}, Vec2i{0, 0}};
	CHECK(static_cast<bool>(sweptIntersect(b0, b1)),
			"two overlapping bodies must collide with no context anywhere");
}

void
testCollisionBasics()
{
	const CollisionMask a = solid(4, 4);
	const CollisionMask b = solid(4, 4);

	// Stationary and coincident: a hit at the first time step.
	{
		const Body b0{&a, Vec2i{10, 10}, Vec2i{10, 10}};
		const Body b1{&b, Vec2i{10, 10}, Vec2i{10, 10}};
		const Impact hit = sweptIntersect(b0, b1);
		CHECK(static_cast<bool>(hit), "coincident bodies collide");
		CHECK(hit.time == 1, "and do so immediately, got %u", hit.time);
	}

	// Stationary and far apart: no hit.
	{
		const Body b0{&a, Vec2i{0, 0}, Vec2i{0, 0}};
		const Body b1{&b, Vec2i{100, 100}, Vec2i{100, 100}};
		CHECK(!sweptIntersect(b0, b1), "distant stationary bodies miss");
	}

	// Closing head-on over one frame: a hit somewhere in the middle, not at
	// the ends. This is the case a non-swept test would miss entirely if they
	// passed through each other.
	{
		const Body b0{&a, Vec2i{0, 0}, Vec2i{40, 0}};
		const Body b1{&b, Vec2i{40, 0}, Vec2i{0, 0}};
		const Impact hit = sweptIntersect(b0, b1);
		CHECK(static_cast<bool>(hit), "closing bodies must collide mid-frame");
		CHECK(hit.time > 1 && hit.time < kMaxTimeValue,
				"impact should be inside the frame, got %u", hit.time);
		// They meet near the middle, so the reported positions should be
		// close together -- that is what the caller places an explosion at.
		const std::int32_t gap = hit.at0.x - hit.at1.x;
		CHECK(gap > -8 && gap < 8, "impact positions should nearly coincide, "
									"gap %ld", static_cast<long>(gap));
	}

	// A body that would pass through if only the endpoints were tested: it
	// starts left of the target and ends right of it, never overlapping at
	// either end.
	{
		const CollisionMask bullet = solid(1, 1);
		const Body shot{&bullet, Vec2i{-20, 0}, Vec2i{20, 0}};
		const Body target{&b, Vec2i{0, 0}, Vec2i{0, 0}};
		CHECK(static_cast<bool>(sweptIntersect(shot, target)),
				"a shot that crosses the target must hit, not tunnel");
	}
}

void
testCollisionIsPerPixelNotBoxes()
{
	// Two rings, one small enough to sit inside the other's hollow centre.
	// Their boxes overlap; no opaque pixel does.
	const CollisionMask outer = ring(9, 9);
	const std::vector<std::uint8_t> dot{1};
	const CollisionMask pixel(Extent2u{1, 1}, Vec2i{0, 0}, dot);

	const Body hole{&outer, Vec2i{0, 0}, Vec2i{0, 0}};
	const Body inside{&pixel, Vec2i{0, 0}, Vec2i{0, 0}};
	CHECK(!sweptIntersect(hole, inside),
			"a pixel in the ring's hollow centre must not collide");

	// Move it onto the rim and it does.
	const Body onRim{&pixel, Vec2i{-4, 0}, Vec2i{-4, 0}};
	CHECK(static_cast<bool>(sweptIntersect(hole, onRim)),
			"the same pixel on the rim must collide");
}

void
testCollisionEdgeCases()
{
	const CollisionMask a = solid(4, 4);
	const CollisionMask b = solid(4, 4);
	const Body b0{&a, Vec2i{0, 0}, Vec2i{0, 0}};
	const Body b1{&b, Vec2i{0, 0}, Vec2i{0, 0}};

	CHECK(!sweptIntersect(b0, b1, 0), "maxTime 0 is no collision");

	// A null mask is a caller bug, but it must not be a read through null.
	const Body none{nullptr, Vec2i{0, 0}, Vec2i{0, 0}};
	CHECK(!sweptIntersect(none, b1), "a missing mask cannot collide");
	CHECK(!sweptIntersect(b0, none), "either way round");
}

// --------------------------------------------------------------------------
// Velocity

void
testVelocityCarriesSubUnitDrift()
{
	// The reason velocity is fixed point with a carried error rather than a
	// rounded integer: a drift slower than one world unit per frame still has
	// to move. A truncating implementation would round it to zero and the
	// object would hang in space forever.
	Velocity v;
	v.setComponents(1, 0);  // 1/32 of a world unit per frame
	CHECK(v.current().x == 1, "a sub-unit component survives being set");

	std::int32_t travelled = 0;
	for (int f = 0; f < 32; ++f)
		travelled += v.advance(1).x;
	CHECK(travelled == 1,
			"1/32 per frame should cover exactly one unit in 32 frames, got %ld",
			static_cast<long>(travelled));

	// ...and the same total whether taken in one step or many, since the
	// error is carried rather than discarded.
	Velocity bulk;
	bulk.setComponents(1, 0);
	CHECK(bulk.advance(32).x == 1, "one 32-frame step covers the same ground");
}

void
testVelocityNegativeEncoding()
{
	// The sign lives in a packed byte, and reconstruction has to recover it
	// exactly -- including the fractional part, which is where the doubled
	// remainder in the high byte earns its keep.
	for (std::int32_t v = -200; v <= 200; ++v)
	{
		Velocity vel;
		vel.setComponents(v, -v);
		const Vec2i got = vel.current();
		CHECK(got.x == v && got.y == -v,
				"components (%ld, %ld) should round-trip, got (%ld, %ld)",
				static_cast<long>(v), static_cast<long>(-v),
				static_cast<long>(got.x), static_cast<long>(got.y));
	}

	// A negative drift accumulates in the right direction.
	Velocity down;
	down.setComponents(0, -1);
	std::int32_t travelled = 0;
	for (int f = 0; f < 32; ++f)
		travelled += down.advance(1).y;
	CHECK(travelled == -1, "negative sub-unit drift should move -1, got %ld",
			static_cast<long>(travelled));
}

void
testVelocityAngles()
{
	// setVector keeps the *facing* as authoritative, so a zero magnitude
	// still remembers which way the object points...
	Velocity aimed;
	aimed.setVector(0, 4);
	CHECK(aimed.travelAngle() == facingToAngle(4),
			"a zero-magnitude vector keeps its facing");

	// ...whereas setComponents derives the angle, and a zero vector reports
	// ARCTAN's "no direction" sentinel rather than "up".
	Velocity stopped;
	stopped.setComponents(0, 0);
	CHECK(stopped.travelAngle() == kFullCircle,
			"a stopped object has no travel angle, got %d",
			stopped.travelAngle());
	CHECK(stopped.isZero(), "and is zero");

	// deltaComponents adds to what is there.
	Velocity v;
	v.setComponents(100, 0);
	v.deltaComponents(-100, 50);
	const Vec2i got = v.current();
	CHECK(got.x == 0 && got.y == 50, "delta should sum, got (%ld, %ld)",
			static_cast<long>(got.x), static_cast<long>(got.y));
}

// --------------------------------------------------------------------------
// Thrust

void
testThrustTakesItsFacingAsAnArgument()
{
	// The whole point of the primitive: thrusting somewhere other than where
	// the ship points needs no save/overwrite/restore of a global. Supox's
	// omni-thrust is "pick a facing delta, call thrust", and this is that
	// call being possible at all.
	constexpr ThrustProfile cruiser{24, 3};

	Velocity forward;
	Velocity backward;
	const SpeedState a = thrust(forward, 0, cruiser, ThrustState{});
	const SpeedState b = thrust(backward, 8, cruiser, ThrustState{});
	CHECK(a == SpeedState::Normal && b == SpeedState::Normal,
			"a single frame of thrust from rest is normal acceleration");

	// Opposite facings give opposite velocities, to within the table's
	// one-count floor asymmetry.
	const Vec2i f = forward.current();
	const Vec2i r = backward.current();
	CHECK(f.y < 0, "facing 0 is up, so thrusting it goes -y (got %ld)",
			static_cast<long>(f.y));
	CHECK(r.y > 0, "facing 8 is down (got %ld)", static_cast<long>(r.y));
	CHECK(f.y + r.y >= -1 && f.y + r.y <= 1,
			"opposite thrusts should cancel to within a count, got %ld",
			static_cast<long>(f.y + r.y));
}

void
testThrustReachesAndHoldsMaxSpeed()
{
	constexpr ThrustProfile cruiser{24, 3};

	Velocity v;
	ThrustState st;
	int frames = 0;
	while (st.speed == SpeedState::Normal && frames < 100)
	{
		st.speed = thrust(v, 0, cruiser, st);
		++frames;
	}
	CHECK(st.speed == SpeedState::AtMax,
			"a cruiser accelerating in a straight line should reach max");
	CHECK(frames > 1 && frames < 100,
			"and should take several frames to do it, took %d", frames);

	// Once there, further thrust along the same heading is a no-op and the
	// state stays put -- this is the early-out the C takes.
	const Vec2i atMax = v.current();
	st.speed = thrust(v, 0, cruiser, st);
	CHECK(st.speed == SpeedState::AtMax, "still at max");
	CHECK(v.current() == atMax, "and the velocity does not creep upward");
}

void
testInertialessThrustIsInstant()
{
	// The Skiff: thrust_increment == max_thrust, so it reaches full speed in
	// one frame and can never be beyond max. The C tests the equality rather
	// than carrying a flag, and so does this.
	constexpr ThrustProfile skiff{40, 40};
	CHECK(skiff.inertialess(), "equal increment and max means inertialess");

	Velocity v;
	const SpeedState s = thrust(v, 4, skiff, ThrustState{});
	CHECK(s == SpeedState::AtMax, "a skiff is at max after one frame");

	// And it turns on a pin: a new facing replaces the vector outright
	// rather than being integrated into it.
	const SpeedState s2 = thrust(v, 12, skiff, ThrustState{s, false});
	CHECK(s2 == SpeedState::AtMax, "still at max after reversing");
	CHECK(v.travelAngle() == facingToAngle(12),
			"and travels the new way immediately");
}

void
testGravityWellAllowsExceedingMax()
{
	constexpr ThrustProfile cruiser{24, 3};

	// Get up to the ship's own maximum first.
	Velocity v;
	ThrustState st;
	for (int i = 0; i < 60 && st.speed == SpeedState::Normal; ++i)
		st.speed = thrust(v, 0, cruiser, st);
	CHECK(st.speed == SpeedState::AtMax, "at max before the well");

	// Inside a well, thrust keeps adding speed past the ship's maximum, up to
	// the hard ceiling. That is what a gravity whip is.
	st.inGravityWell = true;
	const Vec2i before = v.current();
	st.speed = thrust(v, 0, cruiser, st);
	CHECK(st.speed == SpeedState::BeyondMax,
			"a well should push the ship beyond its own maximum");
	const Vec2i after = v.current();
	CHECK(after.y < before.y,
			"and it should actually be faster (%ld -> %ld)",
			static_cast<long>(before.y), static_cast<long>(after.y));
}

// --------------------------------------------------------------------------
// Spawn descriptors

ShipView
cruiserView()
{
	ShipView v;
	v.position = Vec2i{1000, 1000};
	v.facing = 3;
	v.playerNr = 0;
	// NOT the real MISSILE_SPEED, which is 40: human.c:41-44 takes
	// max(MAX_THRUST, DISPLAY_TO_WORLD(10)) and the floor wins. The value
	// here is arbitrary -- spawn functions pass it through untouched, which
	// is exactly what this section tests.
	v.weaponSpeed = 24;
	v.weaponLife = 60;        // MISSILE_LIFE
	v.weaponDamage = 4;       // MISSILE_DAMAGE
	v.weaponHitPoints = 1;    // MISSILE_HITS
	v.muzzleOffset = 42;      // HUMAN_OFFSET
	v.blastOffset = 8;        // NUKE_OFFSET
	return v;
}

void
testSpawningIsRepeatable()
{
	// The property the AI's lookahead needs and the C does not have. In the
	// C, asking "what would I spawn?" runs init_weapon_func for real, and
	// umgah.c:330-341 writes through the shared RaceDescPtr while it does --
	// an HFree plus an HMalloc, every lookahead frame, plus a mutated sprite.
	//
	// Here the question is free to ask. Asking it a hundred times must give
	// the same answer and change nothing.
	const ShipView ship = cruiserView();

	SpawnBuffer first{};
	const std::size_t n = spawnCruiserPrimary(ship, first);
	CHECK(n == 1, "the cruiser fires one missile, got %zu", n);

	for (int i = 0; i < 100; ++i)
	{
		SpawnBuffer again{};
		const std::size_t m = spawnCruiserPrimary(ship, again);
		CHECK(m == n, "spawn count must not drift, got %zu on call %d", m, i);
		CHECK(again[0] == first[0],
				"spawn descriptor must be identical on call %d", i);
	}

	// And the ship it was asked about is untouched -- which the signature
	// already guarantees, but asserting it says why the signature is that
	// shape.
	const ShipView after = cruiserView();
	CHECK(ship.position == after.position && ship.facing == after.facing,
			"asking what would spawn must not change the ship");
}

void
testSpawnCarriesTheShipsParameters()
{
	const ShipView ship = cruiserView();
	SpawnBuffer buf{};
	const std::size_t n = spawnCruiserPrimary(ship, buf);
	CHECK(n == 1, "one spawn");

	const Spawn &s = buf[0];
	CHECK(s.facing == ship.facing, "the missile inherits the facing");
	CHECK(s.speed == 24 && s.life == 60 && s.damage == 4,
			"and the ship's weapon parameters");
	CHECK(s.playerNr == ship.playerNr, "and its owner");

	// The muzzle sits forward of the hotspot along the facing, not on it.
	CHECK(!(s.position == ship.position),
			"the missile appears at the muzzle, not the hotspot");
	const std::int32_t dx = s.position.x - ship.position.x;
	const std::int32_t dy = s.position.y - ship.position.y;
	const std::int64_t dist2 = std::int64_t{dx} * dx + std::int64_t{dy} * dy;
	const std::int64_t want = std::int64_t{displayToWorld(42)}
			* displayToWorld(42);
	// Within a few percent: the offset goes through the 14-bit sine table.
	CHECK(dist2 > want * 9 / 10 && dist2 < want * 11 / 10,
			"muzzle should be ~42 display pixels out, got %lld vs %lld",
			static_cast<long long>(dist2), static_cast<long long>(want));
}

void
testTheTwoShipsDifferWhereTheCDoes()
{
	ShipView ship = cruiserView();
	ship.facing = 7;

	SpawnBuffer cruiser{};
	SpawnBuffer avenger{};
	(void)spawnCruiserPrimary(ship, cruiser);
	(void)spawnAvengerPrimary(ship, avenger);

	// human.c:281 sets index = ShipFacing; ilwrath.c:201 sets index = 0. The
	// nuke sprite is directional and the flame is not, so a rewrite that made
	// these symmetrical would draw the Ilwrath's flame wrong.
	CHECK(cruiser[0].frameIndex == 7,
			"the cruiser's missile frame follows its facing, got %u",
			cruiser[0].frameIndex);
	CHECK(avenger[0].frameIndex == 0,
			"the avenger's flame frame does not, got %u",
			avenger[0].frameIndex);

	// ilwrath.c:203 -- IGNORE_SIMILAR, so a flame stream does not collide
	// with itself. human.c:283 -- flags 0, so missiles do.
	CHECK(!cruiser[0].ignoreSimilar, "cruiser missiles collide with each other");
	CHECK(avenger[0].ignoreSimilar, "flame does not collide with itself");
}

// --------------------------------------------------------------------------
// The battle step

// Hooks are free functions, so the things they need to record go here.
struct Trace
{
	std::vector<int> preOrder;
	std::vector<int> deaths;
	EntityId spawnFrom;
	bool spawnAtHead = false;
	bool spawned = false;
};
Trace g_trace;

// Tags an element so the trace can name it. `mass` is unused by these tests,
// so it doubles as a label.
void
recordPre(Battle &b, EntityId id) noexcept
{
	auto e = b.get(id);
	g_trace.preOrder.push_back(static_cast<int>(e->mass));
}

void
recordDeath(Battle &b, EntityId id) noexcept
{
	auto e = b.get(id);
	g_trace.deaths.push_back(static_cast<int>(e->mass));
}

// A hook that spawns another element the first time it runs.
void
spawnOnce(Battle &b, EntityId id) noexcept
{
	g_trace.preOrder.push_back(static_cast<int>(b.get(id)->mass));
	if (g_trace.spawned)
		return;
	g_trace.spawned = true;

	Element child;
	child.mass = 99;
	child.preProcess = recordPre;
	// PlayerShip so the hook runs on the appearing frame -- what is under test
	// is the catch-up pass, and without the flag the hook would be skipped for
	// the unrelated (and correct) reason that projectiles do not preprocess on
	// the frame they are born.
	child.flags = ElementFlags::FiniteLife | ElementFlags::PlayerShip;
	child.lifeSpan = 5;
	if (g_trace.spawnAtHead)
		g_trace.spawnFrom = b.spawnFront(std::move(child));
	else
		g_trace.spawnFrom = b.spawnBack(std::move(child));
}

Element
plain(int label)
{
	Element e;
	e.mass = label;
	e.preProcess = recordPre;
	// PlayerShip, because that is the flag that makes the preprocess hook run
	// on the appearing frame (process.c:150-151). A projectile's hook is
	// skipped on its first frame; these tests are about ordering, not that.
	e.flags = ElementFlags::PlayerShip;
	return e;
}

void
testStepVisitsInListOrder()
{
	g_trace = Trace{};
	Battle b(1);
	b.spawnBack(plain(1));
	b.spawnBack(plain(2));
	b.spawnBack(plain(3));
	b.step();
	CHECK(g_trace.preOrder == std::vector<int>({1, 2, 3}),
			"preprocess should follow list order");

	// Head insertion puts the newcomer first next frame -- the pkunk.c
	// phoenix ordering.
	g_trace.preOrder.clear();
	b.elements().pushFront(plain(0));
	b.step();
	CHECK(g_trace.preOrder == std::vector<int>({0, 1, 2, 3}),
			"a head-inserted element preprocesses first");
}

void
testMidFrameSpawnIsCaughtUpSameFrame()
{
	// An element spawned during the pre pass has already been walked past.
	// The C gives it a catch-up pass in PostProcessQueue (process.c:844-862)
	// so a weapon created this frame can still act this frame; without it the
	// projectile would sit inert for a frame, which at 24 Hz is visible.
	for (const bool atHead : {false, true})
	{
		g_trace = Trace{};
		g_trace.spawnAtHead = atHead;

		Battle b(1);
		Element shooter = plain(1);
		shooter.preProcess = spawnOnce;
		b.spawnBack(std::move(shooter));
		b.spawnBack(plain(2));

		b.step();
		CHECK(g_trace.spawned, "the hook should have spawned");

		// 99 must appear exactly once: the catch-up pass runs it, and the
		// pre pass must not have run it as well.
		const auto count = static_cast<int>(std::count(
				g_trace.preOrder.begin(), g_trace.preOrder.end(), 99));
		CHECK(count == 1,
				"%s: the mid-frame spawn should preprocess exactly once, got %d",
				atHead ? "head" : "tail", count);
		CHECK(b.elements().size() == 3, "%s: all three should be alive",
				atHead ? "head" : "tail");
	}
}

void
testFiniteLifeExpiresAndCallsDeath()
{
	g_trace = Trace{};
	Battle b(1);

	Element shot = plain(7);
	shot.flags = ElementFlags::FiniteLife | ElementFlags::PlayerShip;
	shot.lifeSpan = 3;
	shot.onDeath = recordDeath;
	b.spawnBack(std::move(shot));

	// Life is spent in the pre pass and death is decided at the *start* of
	// the frame after it reaches zero (process.c:133-141, 180-181). A
	// 3-frame life therefore survives three steps and dies on the fourth.
	b.step();
	b.step();
	b.step();
	CHECK(b.elements().size() == 1, "a 3-frame life survives three steps");
	b.step();
	CHECK(b.elements().empty(), "and is gone on the fourth");
	CHECK(g_trace.deaths == std::vector<int>({7}),
			"its death hook should have run exactly once");
}

void
testMotionIntegratesAndWraps()
{
	Battle b(1);
	Element e;
	e.current = Vec2i{100, 100};
	e.velocity.setComponents(worldToVelocity(10), 0);
	const EntityId id = b.spawnBack(std::move(e));

	// A newly spawned element *does* move on its first step. Appearing
	// suppresses the preprocess hook, not the motion -- process.c:163 gates
	// movement on IGNORE_VELOCITY alone. Getting this wrong costs every
	// projectile its first frame of flight.
	b.step();
	CHECK(b.get(id)->current.x == 110,
			"it should move 10 on its very first step, got %ld",
			static_cast<long>(b.get(id)->current.x));

	b.step();
	CHECK(b.get(id)->current.x == 120, "and 10 a frame after, got %ld",
			static_cast<long>(b.get(id)->current.x));

	// And the arena is a torus. Teleporting means moving BOTH points, the way
	// placeShipAtRandom does: integration adds to `next` (process.c:172-173),
	// so a `current` moved on its own is simply overwritten at the commit.
	// The wrap itself happens at the commit too (process.c:899-916), so
	// mid-frame coordinates may run off the edge -- what matters is what the
	// element's position is when the frame is done.
	b.get(id)->current = Vec2i{kLogSpaceWidth - 5, 0};
	b.get(id)->next = b.get(id)->current;
	b.step();
	CHECK(b.get(id)->current.x < 100,
			"crossing the seam should wrap, got %ld",
			static_cast<long>(b.get(id)->current.x));
}

void
testCollisionPairsAreVisitedOnce()
{
	Battle b(1);
	const std::vector<std::uint8_t> bits(16, 1);
	static const CollisionMask mask(
			Extent2u{4, 4}, Vec2i{2, 2}, bits);

	Element a;
	a.current = Vec2i{500, 500};
	a.mask = &mask;
	a.kind = ElementKind::Weapon;
	a.playerNr = 0;
	// Transient, like a real weapon -- and load-bearing here: an at-rest
	// overlap between two *solid* bodies is the "BAD NEWS" case the step
	// skips, so only a transient can register this stationary hit at all.
	// Two frames of life, because the first is spent before the test runs.
	a.flags = ElementFlags::FiniteLife;
	a.lifeSpan = 2;
	const EntityId ia = b.spawnBack(std::move(a));

	Element c;
	c.current = Vec2i{500, 500};
	c.mask = &mask;
	c.kind = ElementKind::Ship;
	c.playerNr = 1;
	// At least one side must have mass, or the pair is skipped entirely
	// (collide.h:39). Two massless things have no momentum to exchange.
	c.mass = 6;
	const EntityId ic = b.spawnBack(std::move(c));

	b.step();
	CHECK(b.get(ia)->collidedWith == ic, "a should record hitting c");
	CHECK(b.get(ic)->collidedWith == ia, "and c should record hitting a");

	// Two things from the same *ship* that both carry IgnoreSimilar must not
	// hit each other (collide.h:37-38). Owner, not player and not kind: a
	// flame must not burn the Avenger that breathed it, and those are
	// different kinds.
	Battle b2(1);
	const EntityId shipId{1, 1};

	Element f1;
	f1.current = Vec2i{500, 500};
	f1.mask = &mask;
	f1.kind = ElementKind::Weapon;
	f1.playerNr = 0;
	f1.mass = 4;
	f1.owner = shipId;
	f1.flags = ElementFlags::IgnoreSimilar;
	const EntityId if1 = b2.spawnBack(std::move(f1));

	Element f2 = *b2.get(if1);
	f2.flags = ElementFlags::IgnoreSimilar;
	// A *ship*, not another flame -- the kinds differ, which is precisely the
	// case the old same-kind test let through.
	f2.kind = ElementKind::Ship;
	f2.mass = 7;
	const EntityId if2 = b2.spawnBack(std::move(f2));

	b2.step();
	CHECK(!b2.get(if1)->collidedWith.valid(),
			"a flame must not hit the ship that fired it");
	CHECK(!b2.get(if2)->collidedWith.valid(), "either way round");

	// ...but a different ship's flame, same player or not, does hit.
	Battle b3(1);
	Element g1 = *b2.get(if1);
	g1.owner = EntityId{7, 1};
	g1.collidedWith = kNoEntity;
	// Transient for the same reason as above: the target is solid, so the
	// flame must be finite-life for a stationary overlap to be a hit.
	g1.flags = ElementFlags::IgnoreSimilar | ElementFlags::FiniteLife;
	g1.lifeSpan = 2;
	const EntityId ig1 = b3.spawnBack(std::move(g1));

	Element g2 = *b2.get(if2);
	g2.owner = EntityId{9, 1};
	g2.collidedWith = kNoEntity;
	const EntityId ig2 = b3.spawnBack(std::move(g2));

	b3.step();
	CHECK(b3.get(ig1)->collidedWith == ig2,
			"different owners collide even with IgnoreSimilar on both");
}

// --------------------------------------------------------------------------
// Collision response

void
testIsqrtIsFloorSqrt()
{
	CHECK(isqrt(0) == 0, "isqrt(0)");
	CHECK(isqrt(1) == 1, "isqrt(1)");
	CHECK(isqrt(3) == 1, "isqrt floors");
	CHECK(isqrt(4) == 2, "isqrt(4)");
	CHECK(isqrt(0xFFFFFFFFu) == 65535, "isqrt at the top of the range");
	for (std::uint32_t r = 1; r < 1000; ++r)
	{
		CHECK(isqrt(r * r) == r, "isqrt of a perfect square %u", r);
		CHECK(isqrt(r * r - 1) == r - 1, "and just below one");
	}
}

void
testHeadOnCollisionExchangesMomentum()
{
	Element a;
	a.mass = 5;
	a.current = Vec2i{0, 0};
	a.next = Vec2i{100, 0};
	a.velocity.setComponents(worldToVelocity(20), 0);
	a.flags = ElementFlags::PlayerShip;
	a.kind = ElementKind::Ship;

	Element b;
	b.mass = 5;
	b.current = Vec2i{200, 0};
	b.next = Vec2i{110, 0};
	b.velocity.setComponents(-worldToVelocity(20), 0);
	b.flags = ElementFlags::PlayerShip;
	b.kind = ElementKind::Ship;

	const Vec2i beforeA = a.velocity.current();
	applyImpulse(a, b);
	const Vec2i afterA = a.velocity.current();

	CHECK(afterA.x < beforeA.x,
			"a head-on hit should reverse the mover, %ld -> %ld",
			static_cast<long>(beforeA.x), static_cast<long>(afterA.x));

	// Ships are staggered by a collision -- that is the recoil you feel.
	CHECK(a.turnWait == kCollisionTurnWait && a.thrustWait == kCollisionThrustWait,
			"a collision should stagger the ship's controls");
}

void
testGravityMassIsNotPushed()
{
	// A planet pushes; it is not pushed (element.h:198).
	CHECK(isGravityMass(101), "mass above 100 is a gravity mass");
	CHECK(!isGravityMass(10), "a ship is not");

	Element ship;
	ship.mass = 5;
	ship.current = Vec2i{0, 0};
	ship.next = Vec2i{100, 0};
	ship.velocity.setComponents(worldToVelocity(20), 0);
	ship.kind = ElementKind::Ship;

	Element planet;
	planet.mass = 200;
	planet.current = Vec2i{200, 0};
	planet.next = Vec2i{200, 0};
	planet.kind = ElementKind::Planet;

	applyImpulse(ship, planet);
	CHECK(planet.velocity.isZero(), "the planet must not move");
	CHECK(!ship.velocity.isZero(), "the ship must");
}

void
testStuckPairIsWorkedApart()
{
	// Two elements that did not move cannot exchange momentum, so the C marks
	// them DefyPhysics instead. If they were *already* defying, it zeroes
	// them and skews the impact axis by an octant, which is what eventually
	// separates two stuck objects rather than leaving them welded.
	Element a;
	a.mass = 5;
	a.current = a.next = Vec2i{100, 100};
	a.kind = ElementKind::Ship;

	Element b = a;
	b.current = b.next = Vec2i{100, 100};

	applyImpulse(a, b);
	CHECK(any(a.flags & ElementFlags::DefyPhysics),
			"a stationary pair should defy physics rather than exchange nothing");
	CHECK(any(b.flags & ElementFlags::DefyPhysics), "both of them");

	// Second time round, already defying: velocities are zeroed and the pair
	// gets pushed apart along a skewed axis.
	applyImpulse(a, b);
	CHECK(!a.velocity.isZero() || !b.velocity.isZero(),
			"an already-stuck pair should be given a way out");
}

void
testDeriveSpeedStateFromVelocity()
{
	// The mechanism the plan wants instead of hand-patched flags. Not yet
	// wired into applyImpulse -- see the note in Impulse.hpp -- but available
	// so the switch is a one-liner.
	constexpr ThrustProfile cruiser{24, 3};

	Velocity slow;
	slow.setComponents(worldToVelocity(5), 0);
	CHECK(deriveSpeedState(slow, cruiser) == SpeedState::Normal,
			"below max is Normal");

	Velocity fast;
	fast.setComponents(worldToVelocity(100), 0);
	CHECK(deriveSpeedState(fast, cruiser) == SpeedState::BeyondMax,
			"well above max is BeyondMax");
}

// --------------------------------------------------------------------------
// Ships

EntityId
addShip(Battle &b, const ShipSpec &data, Vec2i at, int facing, int player)
{
	Element e;
	e.kind = ElementKind::Ship;
	e.flags = ElementFlags::PlayerShip;
	e.current = at;
	e.next = at;
	e.facing = facing;
	e.playerNr = player;
	e.mass = data.mass;
	e.ship.spec = &data;
	return b.spawnBack(std::move(e));
}

void
testShipInitialisesFromItsDescriptor()
{
	Battle b(1);
	const EntityId id = addShip(b, earthlingCruiser(), Vec2i{1000, 1000}, 0, 0);
	b.get(id)->preProcess = shipPreProcess;
	b.get(id)->postProcess = shipPostProcess;

	b.step();
	auto e = b.get(id);
	CHECK(e->ship.crew == 18, "the cruiser starts with 18 crew, got %ld",
			static_cast<long>(e->ship.crew));
	CHECK(e->ship.energy == 18, "and 18 energy, got %ld",
			static_cast<long>(e->ship.energy));
}

void
testTurningIsGatedByTurnWait()
{
	Battle b(1);
	// The Avenger turns every 2 frames; the Cruiser every 1.
	const EntityId slow = addShip(b, ilwrathAvenger(), Vec2i{1000, 1000}, 0, 0);
	b.get(slow)->preProcess = shipPreProcess;
	b.get(slow)->postProcess = shipPostProcess;

	b.step();  // Appearing frame: input is not latched
	b.get(slow)->ship.input = ShipInput::Right;

	// turnWait N means a turn every N+1 frames, not every N: the counter is
	// set to N *after* a turn and has to reach zero again (ship.c:238-253).
	// The Avenger's 2 therefore turns on frames 1, 4, 7...
	const int start = b.get(slow)->facing;
	b.step();
	CHECK(b.get(slow)->facing == normalizeFacing(start + 1),
			"the first turn should happen immediately");
	b.step();
	b.step();
	CHECK(b.get(slow)->facing == normalizeFacing(start + 1),
			"and the next two frames should be spent waiting");
	b.step();
	CHECK(b.get(slow)->facing == normalizeFacing(start + 2),
			"then turn again on the fourth");
}

void
testFiringSpendsEnergyAndRespectsCooldown()
{
	Battle b(1);
	const EntityId id = addShip(b, earthlingCruiser(), Vec2i{2000, 2000}, 0, 0);
	b.get(id)->preProcess = shipPreProcess;
	b.get(id)->postProcess = shipPostProcess;

	b.step();
	CHECK(b.elements().size() == 1, "just the ship so far");

	b.get(id)->ship.input = ShipInput::Weapon;
	b.step();
	CHECK(b.elements().size() == 2, "firing should have spawned a missile");
	CHECK(b.get(id)->ship.energy == 18 - 9,
			"and spent 9 energy, got %ld",
			static_cast<long>(b.get(id)->ship.energy));

	// weapon.wait is 10, so holding the trigger must not fire again yet.
	b.step();
	CHECK(b.elements().size() == 2,
			"the cooldown should block a second shot, got %d elements and "
			"weaponCounter %ld",
			static_cast<int>(b.elements().size()),
			static_cast<long>(b.get(id)->ship.weaponCounter));

	// With the energy drained below the cost, it must not fire at all -- and
	// must not start a cooldown either.
	Battle c(1);
	const EntityId poor = addShip(c, earthlingCruiser(), Vec2i{2000, 2000}, 0, 0);
	c.get(poor)->preProcess = shipPreProcess;
	c.get(poor)->postProcess = shipPostProcess;
	c.step();
	c.get(poor)->ship.energy = 8;   // one short of the 9-point cost
	c.get(poor)->ship.energyCounter = 5;  // ...and hold off regen, which
										  // would otherwise top it up first
	c.get(poor)->ship.input = ShipInput::Weapon;
	c.step();
	CHECK(c.elements().size() == 1, "a ship that cannot afford the shot "
									"must not fire");
	CHECK(c.get(poor)->ship.energy == 8,
			"and must not be charged for it, got %ld",
			static_cast<long>(c.get(poor)->ship.energy));
}

void
testMissileFliesAndExpires()
{
	Battle b(1);
	const EntityId id = addShip(b, earthlingCruiser(), Vec2i{4000, 4000}, 0, 0);
	b.get(id)->preProcess = shipPreProcess;
	b.get(id)->postProcess = shipPostProcess;
	b.step();

	b.get(id)->ship.input = ShipInput::Weapon;
	b.step();
	b.get(id)->ship.input = ShipInput::None;
	CHECK(b.elements().size() == 2, "one missile");

	// Find it and watch it move.
	EntityId shot;
	for (EntityId e = b.elements().front(); e.valid(); e = b.elements().next(e))
		if (b.get(e)->kind == ElementKind::Weapon)
			shot = e;
	CHECK(shot.valid(), "the missile should be in the list");

	const Vec2i first = b.get(shot)->current;
	b.step();
	const Vec2i second = b.get(shot)->current;
	CHECK(!(first == second), "the missile should move");
	CHECK(second.y < first.y, "and facing 0 is up, so it goes -y");

	// MISSILE_LIFE is 60, so it is gone well before 100 frames.
	for (int i = 0; i < 100 && b.elements().size() > 1; ++i)
		b.step();
	CHECK(b.elements().size() == 1, "the missile should expire");
}

void
testFiringPostponesEnergyRegen()
{
	// Every successful energy spend re-arms the regeneration countdown
	// (status.c:317-323), so a ship that fires continuously does not
	// regenerate while it can still afford the shots. The Avenger is the
	// sharpest case: cost 1, weapon.wait 0, regen 4 every 4 -- without the
	// re-arm its flame is close to self-sustaining instead of a 16-frame
	// burst.
	Battle b(1);
	const EntityId id = addShip(b, ilwrathAvenger(), Vec2i{2000, 2000}, 0, 0);
	b.get(id)->preProcess = shipPreProcess;
	b.get(id)->postProcess = shipPostProcess;
	b.step();  // Appearing frame

	b.get(id)->ship.input = ShipInput::Weapon;
	for (int i = 0; i < 6; ++i)
		b.step();

	// Six shots, no regeneration mixed in: 16 - 6, exactly.
	CHECK(b.get(id)->ship.energy == 16 - 6,
			"six frames of flame should cost six energy with no regen, got %ld",
			static_cast<long>(b.get(id)->ship.energy));
}

int g_specialFires = 0;

void
countingSpecial(Battle &b, EntityId id) noexcept
{
	auto e = b.get(id);
	if (e == nullptr)
		return;
	++g_specialFires;
	e->ship.specialCounter = e->ship.spec->special.wait;
}

void
testSpecialFiresTheFrameItsCounterExpires()
{
	// The C decrements special_counter and then lets the ship code see the
	// result (ship.c:342-346), so a held special re-fires every special.wait
	// frames -- the frame the counter reaches zero, not the one after.
	static ShipSpec d = [] {
		ShipSpec d_ = earthlingCruiser();
		d_.special.wait = 3;
		d_.special.energyCost = 0;
		d_.special.hook = countingSpecial;
		return d_;
	}();

	Battle b(1);
	const EntityId id = addShip(b, d, Vec2i{2000, 2000}, 0, 0);
	b.get(id)->preProcess = shipPreProcess;
	b.get(id)->postProcess = shipPostProcess;
	b.step();  // Appearing frame

	g_specialFires = 0;
	b.get(id)->ship.input = ShipInput::Special;
	for (int i = 0; i < 4; ++i)
		b.step();

	// Fires on the 1st step (counter 0) and again on the 4th, when the
	// counter set to 3 has just been decremented to 0 -- a period of
	// special.wait, not special.wait + 1.
	CHECK(g_specialFires == 2,
			"a held special with wait 3 should fire twice in 4 frames, got %d",
			g_specialFires);
}

void
testOpposingMissilesDestroyEachOther()
{
	// A weapon's mass is its damage (weapon.c:101), and CollisionPossible
	// skips a pair only when BOTH masses are zero (collide.h:38) -- so shots
	// from different ships collide and kill each other in flight. This is
	// the mechanism behind flame-intercepts-nuke; a weapon spawned massless
	// silently turns it off.
	static CollisionMask shotMask = solid(3, 3);
	static ShipSpec d = [] {
		ShipSpec d_ = earthlingCruiser();
		d_.weapon.masks = std::span<const CollisionMask>(&shotMask, 1);
		return d_;
	}();

	Battle b(1);
	// Far enough apart that the missiles meet in the middle long before
	// either could reach the opposing ship.
	const EntityId a = addShip(b, d, Vec2i{4000, 6000}, 0, 0);
	const EntityId c = addShip(b, d, Vec2i{4000, 2000}, 8, 1);
	for (const EntityId id : {a, c})
	{
		b.get(id)->preProcess = shipPreProcess;
		b.get(id)->postProcess = shipPostProcess;
	}
	b.step();  // Appearing frame

	b.get(a)->ship.input = ShipInput::Weapon;
	b.get(c)->ship.input = ShipInput::Weapon;
	b.step();
	b.get(a)->ship.input = ShipInput::None;
	b.get(c)->ship.input = ShipInput::None;

	std::size_t weapons = 0;
	for (EntityId e = b.elements().front(); e.valid(); e = b.elements().next(e))
		if (b.get(e)->kind == ElementKind::Weapon)
			++weapons;
	CHECK(weapons == 2, "both ships should have fired, got %zu", weapons);

	// The nukes close at 80+ world units a frame across a ~3300-unit gap:
	// they must meet and annihilate well before frame 40, and long before
	// either 60-frame life expires or either ship is reached.
	for (int i = 0; i < 40; ++i)
		b.step();

	weapons = 0;
	for (EntityId e = b.elements().front(); e.valid(); e = b.elements().next(e))
		if (b.get(e)->kind == ElementKind::Weapon)
			++weapons;
	CHECK(weapons == 0,
			"the missiles should have destroyed each other mid-flight, "
			"%zu still alive", weapons);
	CHECK(b.get(a)->ship.crew == 18 && b.get(c)->ship.crew == 18,
			"and neither ship should have been hit, got %ld and %ld",
			static_cast<long>(b.get(a)->ship.crew),
			static_cast<long>(b.get(c)->ship.crew));
}

void
testTheTwoShipsFeelDifferent()
{
	// Not a rendering claim -- these are the numbers that make the Avenger
	// play nothing like the Cruiser, and they come straight from the C.
	const ShipSpec &cruiser = earthlingCruiser();
	const ShipSpec &avenger = ilwrathAvenger();

	CHECK(cruiser.thrustWait == 4 && avenger.thrustWait == 0,
			"the Avenger accelerates every frame, the Cruiser every fourth");
	CHECK(cruiser.weapon.wait == 10 && avenger.weapon.wait == 0,
			"the Avenger's flame is continuous, the Cruiser's missiles are not");
	CHECK(cruiser.turnWait == 1 && avenger.turnWait == 2,
			"but the Cruiser turns twice as fast");
	CHECK(cruiser.weapon.energyCost == 9 && avenger.weapon.energyCost == 1,
			"and pays 9 a shot against the Avenger's 1");
}

// --------------------------------------------------------------------------
// Gravity and the battlefield

EntityId
addPlanet(Battle &b, const CollisionMask &m, Vec2i at)
{
	Element p;
	p.kind = ElementKind::Planet;
	p.playerNr = -1;
	p.hitPoints = 200;
	p.mass = 200;
	p.lifeSpan = 2;
	p.mask = &m;
	p.current = at;
	p.next = at;
	p.postProcess = planetPostProcess;
	return b.spawnBack(std::move(p));
}

void
testGravityMassThreshold()
{
	CHECK(kGravityMass == 100, "MAX_SHIP_MASS * 10");
	CHECK(isGravitySource(200), "a planet (mass 200) is a gravity source");
	CHECK(!isGravitySource(6), "the Cruiser is not");
	CHECK(!isGravitySource(3), "nor is an asteroid");
	CHECK(!isGravitySource(99), "nor is anything below the threshold");

	// The `+ 1` gravity.c applies before every test. A ship running away is
	// set to exactly MAX_SHIP_MASS * 10 (battle.c:92), which fails the plain
	// GRAVITY_MASS macro but passes this -- and since gravity skips any pair
	// that agrees, that is precisely what stops a planet dragging in a ship
	// that is trying to escape.
	CHECK(isGravitySource(kGravityMass),
			"a fleeing ship counts as a source, so nothing pulls on it");
}

void
testGravityPullsTowardTheSource()
{
	const CollisionMask m = solid(4, 4);
	Battle b(1);
	const EntityId planet = addPlanet(b, m, Vec2i{4000, 4000});

	// 100 world units to the planet's right, well inside the 1020-unit disc.
	const EntityId ship =
			addShip(b, earthlingCruiser(), Vec2i{4100, 4000}, 0, 0);
	b.get(ship)->mask = &m;
	b.get(ship)->ship.speed = SpeedState::AtMax;

	CHECK(!calculateGravity(b, planet),
			"the source itself is never in a well");

	const Vec2i v = b.get(ship)->velocity.current();
	CHECK(v.x < 0, "the ship should be pulled back toward the planet, got %ld",
			static_cast<long>(v.x));
	CHECK(v.y == 0, "and straight along the axis, got %ld",
			static_cast<long>(v.y));
	CHECK(b.get(ship)->ship.inGravityWell,
			"and should be flagged as being in the well");
	CHECK(b.get(ship)->ship.speed == SpeedState::Normal,
			"gravity.c:136 clears at-max so the ship may accelerate again");

	// And the other direction: asked of the light element, it reports the well.
	CHECK(calculateGravity(b, ship), "the ship should know it is in a well");
}

void
testGravityHasAHardEdge()
{
	const CollisionMask m = solid(4, 4);

	// GRAVITY_THRESHOLD is 255 *display* pixels, and ONE_SHIFT is 2, so the
	// disc is 1020 world units. There is no falloff inside it and nothing at
	// all outside -- the DIFUSE_GRAVITY block that would have smoothed this is
	// commented out in the C (gravity.c:96-111).
	for (const auto &[dx, pulled] :
			{std::pair{1020, true}, std::pair{1024, false}})
	{
		Battle b(1);
		const EntityId planet = addPlanet(b, m, Vec2i{4000, 4000});
		const EntityId ship =
				addShip(b, earthlingCruiser(), Vec2i{4000 + dx, 4000}, 0, 0);
		b.get(ship)->mask = &m;

		(void)calculateGravity(b, planet);
		const bool moved = !b.get(ship)->velocity.isZero();
		CHECK(moved == pulled, "at %d world units the pull should be %s", dx,
				pulled ? "on" : "off");
	}
}

void
testFleeingShipIsImmuneToGravity()
{
	const CollisionMask m = solid(4, 4);
	Battle b(1);
	const EntityId planet = addPlanet(b, m, Vec2i{4000, 4000});
	const EntityId ship =
			addShip(b, earthlingCruiser(), Vec2i{4100, 4000}, 0, 0);
	b.get(ship)->mask = &m;
	b.get(ship)->mass = kGravityMass;  // DoRunAway, battle.c:92

	(void)calculateGravity(b, planet);
	CHECK(b.get(ship)->velocity.isZero(),
			"a ship at mass 100 reads as a source, so the planet skips it");
}

void
testAsteroidsSpawnOnAnEdgeAndRepeatably()
{
	const CollisionMask m = solid(4, 4);

	Battle b(7);
	Battle c(7);
	for (int i = 0; i < kNumAsteroids; ++i)
	{
		const EntityId a = spawnAsteroid(b, &m);
		const EntityId a2 = spawnAsteroid(c, &m);

		auto e = b.get(a);
		const bool onEdge = e->current.x == 0 || e->current.x == kLogSpaceWidth
				|| e->current.y == 0 || e->current.y == kLogSpaceHeight;
		CHECK(onEdge, "asteroid %d should enter from an edge, got (%ld,%ld)", i,
				static_cast<long>(e->current.x),
				static_cast<long>(e->current.y));
		CHECK(!e->velocity.isZero(), "and should be moving");

		// Same seed, same asteroid: the six draws happen in a fixed order.
		auto e2 = c.get(a2);
		CHECK(e->current == e2->current && e->facing == e2->facing
						&& e->thrustWait == e2->thrustWait,
				"asteroid %d should be identical from the same seed", i);
	}
}

void
testAsteroidTumbles()
{
	const CollisionMask m = solid(4, 4);
	Battle b(3);
	const EntityId a = spawnAsteroid(b, &m);

	// thrust_wait is the spin period, not a thrust delay (misc.c:117-126), and
	// like turn_wait it means "every N+1 frames".
	const int period = static_cast<int>(b.get(a)->thrustWait);
	const int start = b.get(a)->facing;

	// period + 1 frames of stillness, not period: the first step is the
	// asteroid's appearing frame and an asteroid is not a PLAYER_SHIP, so its
	// preprocess hook does not run at all that frame (process.c:150-154).
	for (int i = 0; i < period + 1; ++i)
		b.step();
	CHECK(b.get(a)->facing == start, "it should hold still for %d frames",
			period + 1);

	b.step();
	CHECK(b.get(a)->facing != start, "then rotate one frame index");
}

void
testDestroyedAsteroidIsReplaced()
{
	// The field's population is constant because the loop is closed: an
	// asteroid's death leaves rubble, and the rubble's death spawns a fresh
	// asteroid (misc.c:80-105, 130-201). Break either link and the field
	// empties out over a long match.
	const CollisionMask m = solid(4, 4);
	Battle b(11);
	(void)spawnAsteroid(b, &m);
	CHECK(b.elements().size() == 1, "one asteroid to start");

	// do_damage's kill (misc.c:220-222).
	for (EntityId e = b.elements().front(); e.valid(); e = b.elements().next(e))
	{
		b.get(e)->hitPoints = 0;
		b.get(e)->lifeSpan = 0;
		b.get(e)->flags |= ElementFlags::NonSolid;
	}

	int asteroids = 0;
	for (int i = 0; i < 12; ++i)
	{
		b.step();
		asteroids = 0;
		for (EntityId e = b.elements().front(); e.valid();
				e = b.elements().next(e))
			if (b.get(e)->kind == ElementKind::Asteroid)
				++asteroids;
		if (asteroids == 1)
			break;
	}
	CHECK(asteroids == 1, "the field should refill itself, got %d asteroids",
			asteroids);
}

void
testPlanetPlacementAvoidsEverything()
{
	const CollisionMask m = solid(4, 4);
	Battle b(5);

	// A ship already on the field. The planet must not land in its lap, and
	// must not land close enough that the ship starts the match in a well.
	const EntityId ship =
			addShip(b, earthlingCruiser(), Vec2i{4000, 4000}, 0, 0);
	b.get(ship)->mask = &m;

	const EntityId planet = spawnPlanet(b, &m);
	CHECK(b.get(planet)->mass == 200,
			"mass is assigned after placement, got %ld",
			static_cast<long>(b.get(planet)->mass));
	CHECK(!timeSpaceMatterConflict(b, planet),
			"the planet should not overlap the ship");
	CHECK(!calculateGravity(b, ship),
			"and the ship should not start inside its well");
}

// --------------------------------------------------------------------------
// Damage

void
testDeltaCrewReportsDeathOnTheExactHit()
{
	Battle b(1);
	const EntityId id = addShip(b, earthlingCruiser(), Vec2i{1000, 1000}, 0, 0);
	b.get(id)->preProcess = shipPreProcess;
	b.step();  // the appearing frame is what loads crew from the descriptor

	CHECK(b.get(id)->ship.crew == 18, "the Cruiser starts with 18 crew, got %ld",
			static_cast<long>(b.get(id)->ship.crew));

	CHECK(deltaCrew(*b.get(id), -4), "losing 4 of 18 is survivable");
	CHECK(b.get(id)->ship.crew == 14, "and leaves 14, got %ld",
			static_cast<long>(b.get(id)->ship.crew));

	// status.c:357 compares with a strict `>`, so losing exactly what is left
	// is death, not a ship sitting at zero crew. Off by one here and a ship
	// survives its own destruction.
	CHECK(!deltaCrew(*b.get(id), -14), "losing exactly the remainder is death");
	CHECK(b.get(id)->ship.crew == 0, "and leaves nothing");

	// Repair cannot exceed the maximum.
	CHECK(deltaCrew(*b.get(id), 100), "repair always succeeds");
	CHECK(b.get(id)->ship.crew == 18, "and is capped at max, got %ld",
			static_cast<long>(b.get(id)->ship.crew));
}

void
testPlanetsTakeNoDamage()
{
	Element planet;
	planet.kind = ElementKind::Planet;
	planet.mass = 200;
	planet.hitPoints = 200;
	planet.lifeSpan = 2;

	doDamage(planet, 50);
	CHECK(planet.hitPoints == 200, "a planet is not damageable, got %ld",
			static_cast<long>(planet.hitPoints));
	CHECK(planet.lifeSpan == 2, "and is certainly not killable");

	// The same call, asked of a ship that has fled to mass 100. isGravityMass
	// is the predicate *without* gravity.c's `+ 1`, so it stays damageable
	// even while gravity treats it as a source.
	Element fleeing;
	fleeing.kind = ElementKind::Ship;
	fleeing.mass = kGravityMass;
	fleeing.hitPoints = 10;
	doDamage(fleeing, 4);
	CHECK(fleeing.hitPoints == 6,
			"a fleeing ship is still damageable, got %ld",
			static_cast<long>(fleeing.hitPoints));
}

void
testMissileDamagesAndSpendsItself()
{
	const CollisionMask m = solid(8, 8);
	Battle b(1);

	// A local copy of the descriptor so the missile gets a collision mask.
	// The real one comes from content; ShipSpec::weapon.masks is null until
	// something loads it, and a weapon with no mask cannot hit anything.
	static const ShipSpec cruiser = [&m] {
		ShipSpec d = earthlingCruiser();
		d.weapon.masks = std::span<const CollisionMask>(&m, 1);
		return d;
	}();

	// Two ships nose to nose, so the Cruiser's missile cannot miss.
	const EntityId gunner = addShip(b, cruiser, Vec2i{4000, 4000}, 0, 0);
	b.get(gunner)->preProcess = shipPreProcess;
	b.get(gunner)->postProcess = shipPostProcess;
	b.get(gunner)->mask = &m;

	// 400 world units away, not 100. HUMAN_OFFSET is 42 *display* pixels,
	// which is 168 world units, so the Cruiser's missile is born further from
	// the hull than a closer target would be -- it would spawn already past
	// it and sail off having never touched anything.
	const EntityId target =
			addShip(b, ilwrathAvenger(), Vec2i{4000, 3600}, 8, 1);
	b.get(target)->preProcess = shipPreProcess;
	b.get(target)->mask = &m;
	b.step();

	const std::int32_t before = b.get(target)->ship.crew;
	CHECK(before == 22, "the Avenger starts with 22 crew, got %ld",
			static_cast<long>(before));

	b.get(gunner)->ship.input = ShipInput::Weapon;
	b.step();
	b.get(gunner)->ship.input = ShipInput::None;

	// The missile flies -40 a frame from y=3832, so it reaches y=3600 in about
	// six. MISSILE_DAMAGE is 4.
	for (int i = 0; i < 12 && b.get(target)->ship.crew == before; ++i)
		b.step();

	CHECK(b.get(target)->ship.crew == before - 4,
			"the missile should cost 4 crew, got %ld",
			static_cast<long>(b.get(target)->ship.crew));

	// One more frame: the walk preprocessed the missile before its hit, so
	// its zeroed life is only seen -- and the wreck reaped -- on the next
	// death check, exactly as in the C.
	b.step();

	int weapons = 0;
	int blasts = 0;
	for (EntityId e = b.elements().front(); e.valid(); e = b.elements().next(e))
	{
		if (b.get(e)->kind == ElementKind::Weapon)
			++weapons;
		if (b.get(e)->kind == ElementKind::Blast)
			++blasts;
	}
	CHECK(weapons == 0, "the missile should be spent, got %d", weapons);
	CHECK(blasts == 1, "and should have left a blast, got %d", blasts);
}

void
testFlyingIntoAPlanetCostsCrewOverFour()
{
	const CollisionMask m = solid(8, 8);
	Battle b(1);

	Element planet;
	planet.kind = ElementKind::Planet;
	planet.playerNr = -1;
	planet.mass = 200;
	planet.hitPoints = 200;
	planet.lifeSpan = 2;
	planet.mask = &m;
	planet.current = Vec2i{4000, 4000};
	planet.next = planet.current;
	planet.onCollision = solidCollision;
	(void)b.spawnBack(std::move(planet));

	// Spawned clear of the planet, then moved into it. Starting them on top of
	// each other is not a shortcut: the planet's collision is resolved before
	// the ship's own first preprocess has loaded its crew from the descriptor,
	// so the ship takes damage while still at zero and is destroyed on frame
	// one. The C has the same ordering, and avoids it the same way -- by never
	// placing a ship inside anything (misc.c:63-70, ship.c:480).
	const EntityId ship = addShip(b, earthlingCruiser(), Vec2i{5000, 5000}, 0, 0);
	b.get(ship)->preProcess = shipPreProcess;
	b.get(ship)->mask = &m;
	b.get(ship)->onCollision = solidCollision;
	b.step();

	CHECK(b.get(ship) != nullptr, "the ship should have survived setup");
	if (b.get(ship) == nullptr)
		return;

	const std::int32_t before = b.get(ship)->ship.crew;

	// Fly into it rather than teleporting into overlap. A pair already
	// overlapping at rest is the "BAD NEWS" case the step deliberately skips
	// (process.c:397-416) -- an embedded ship takes no new damage -- so the
	// contact has to happen mid-motion, the way it does in play.
	b.get(ship)->current = Vec2i{4000, 4064};
	b.get(ship)->next = b.get(ship)->current;
	b.get(ship)->velocity.setComponents(0, -worldToVelocity(40));
	b.step();
	CHECK(b.get(ship) != nullptr, "and should survive one planet graze");
	if (b.get(ship) == nullptr)
		return;

	// ship.c:364-367 computes hit_points >> 2 with a floor of 1 -- and for a
	// PLAYER_SHIP, hit_points IS crew_level: one union field
	// (element.h:126-133). An 18-crew Cruiser therefore pays 4 crew for a
	// planet graze, not 1. The earlier version of this test asserted 1,
	// reasoning from the rewrite's own split fields instead of the union.
	CHECK(b.get(ship)->ship.crew == before - (before >> 2),
			"hitting a planet should cost crew/4 (%ld), got %ld (was %ld)",
			static_cast<long>(before >> 2),
			static_cast<long>(b.get(ship)->ship.crew),
			static_cast<long>(before));
}

void
testOverlappingShipsSeparateInsteadOfSticking()
{
	// Two ships interpenetrating -- the tail of a collision just resolved --
	// are not a new collision. The C skips such a pair outright ("BAD NEWS",
	// process.c:397-416, 509-515) and lets the previous impulse carry them
	// apart. Processing it instead is how ships weld together: the rewind to
	// impact time 1 freezes both where they stand, the scrape-promotion in
	// applyImpulse reflects their separating velocities back inward, and the
	// frozen pair does it all again next frame, forever.
	static const CollisionMask m = solid(8, 8);
	Battle b(1);

	Element a;
	a.kind = ElementKind::Ship;
	a.playerNr = 0;
	a.mass = 6;
	a.mask = &m;
	a.current = a.next = Vec2i{4000, 4000};
	const EntityId ia = b.spawnBack(std::move(a));

	Element c;
	c.kind = ElementKind::Ship;
	c.playerNr = 1;
	c.mass = 6;
	c.mask = &m;
	c.current = c.next = Vec2i{6000, 6000};
	const EntityId ic = b.spawnBack(std::move(c));

	// Spawned far apart and established first. The distinction is the C's:
	// an APPEARING element found overlapping something is EXECUTED on the
	// spot (process.c:427-449), so the skip under test here applies only to
	// elements past their spawn frame -- which post-impact ships always are.
	b.step();

	// Now manufacture the post-impact state: overlapping -- 16 world units
	// is 4 display pixels, half a mask -- and moving apart, exactly what an
	// impact response leaves behind.
	b.get(ia)->current = b.get(ia)->next = Vec2i{4000, 4000};
	b.get(ia)->velocity.setComponents(-worldToVelocity(20), 0);
	b.get(ic)->current = b.get(ic)->next = Vec2i{4016, 4000};
	b.get(ic)->velocity.setComponents(worldToVelocity(20), 0);

	b.step();
	CHECK(b.collisions().empty(), "an at-rest overlap is not a collision");
	CHECK(b.get(ia)->current.x == 3980,
			"the left ship keeps its full motion, got %ld",
			static_cast<long>(b.get(ia)->current.x));
	CHECK(b.get(ic)->current.x == 4036,
			"and the right ship its own, got %ld",
			static_cast<long>(b.get(ic)->current.x));

	// And they stay separated: no re-collision as they clear each other.
	for (int i = 0; i < 10; ++i)
	{
		b.step();
		CHECK(b.collisions().empty(),
				"separating ships must not collide again (frame %d)", i);
	}
	CHECK(b.get(ic)->current.x - b.get(ia)->current.x > 32 * kScaledOne,
			"ten frames later they are well clear of each other");
}

void
testShipShotMidFlightKeepsItsMotion()
{
	// A ship hit by a weapon is not stopped by it. In the C only an element
	// whose own collision_func raised COLLISION is moved to the impact point
	// (process.c:586-596), and a ship's does so only when the other party is
	// solid (ship.c:356-358). Halting the target at the impact point reads on
	// screen as the ship hanging in a stream of fire.
	static const CollisionMask m = solid(8, 8);
	Battle b(1);

	Element ship;
	ship.kind = ElementKind::Ship;
	ship.flags = ElementFlags::PlayerShip;
	ship.playerNr = 0;
	ship.mass = 6;
	ship.mask = &m;
	ship.current = ship.next = Vec2i{4000, 4000};
	const EntityId is = b.spawnBack(std::move(ship));

	// A stationary shot in the ship's path. Zero damage, so the run is about
	// motion, not crew -- and damage IS mass (weapon.c:101,144), so a
	// zero-damage shot is a massless one. The pair still collides because
	// the ship's own mass satisfies CollisionPossible.
	Element shot;
	shot.kind = ElementKind::Weapon;
	shot.flags = ElementFlags::FiniteLife;
	shot.lifeSpan = 20;
	shot.playerNr = 1;
	shot.mass = 0;
	shot.mask = &m;
	shot.current = shot.next = Vec2i{4200, 4000};
	shot.onCollision = weaponCollision;
	const EntityId iw = b.spawnBack(std::move(shot));

	b.step();  // spawn frame: 50 display pixels apart, nothing touches

	b.get(is)->velocity.setComponents(worldToVelocity(80), 0);
	bool hit = false;
	for (int i = 0; i < 6 && !hit; ++i)
	{
		const std::int32_t beforeX = b.get(is)->current.x;
		b.step();
		if (!b.collisions().empty())
		{
			hit = true;
			CHECK(b.get(is)->current.x == beforeX + 80,
					"the ship keeps its full motion through a weapon hit, "
					"got %ld from %ld",
					static_cast<long>(b.get(is)->current.x),
					static_cast<long>(beforeX));
		}
	}
	CHECK(hit, "the ship should have crossed the shot");

	const Vec2i v = b.get(is)->velocity.current();
	CHECK(velocityToWorld(v.x) == 80,
			"and its velocity untouched -- weapons carry no impulse, got %ld",
			static_cast<long>(velocityToWorld(v.x)));
	// The shot is spent AND gone: weapon_collision marks a missile
	// DISAPPEARING (weapon.c:175-177) and the post walk removes such
	// elements the same frame (process.c:873-879) -- a dead missile is never
	// drawn at its impact point. (The flame is the exception: its wrapper
	// clears the flag and it lingers one frame, flameCollision.)
	CHECK(b.get(iw) == nullptr,
			"a spent missile is reaped on the frame it hit");
}

void
testToughWeaponPiercesWeakOne()
{
	// The pierce rule (weapon.c:161-164): a weapon whose hit points exceed a
	// surviving finite target's mass ploughs through it and keeps flying.
	// Nuke and flame, at one hit point each, can never do this -- Chmmr
	// zapsats live on it.
	static const CollisionMask m = solid(3, 3);
	Battle b(1);

	Element tough;
	tough.kind = ElementKind::Weapon;
	tough.flags = ElementFlags::FiniteLife;
	tough.lifeSpan = 30;
	tough.playerNr = 0;
	tough.hitPoints = 3;
	tough.mass = 2;         // damage IS mass
	tough.mask = &m;
	tough.current = tough.next = Vec2i{3800, 4000};
	tough.velocity.setComponents(worldToVelocity(40), 0);
	tough.onCollision = weaponCollision;
	const EntityId it = b.spawnBack(std::move(tough));

	Element weak;
	weak.kind = ElementKind::Weapon;
	weak.flags = ElementFlags::FiniteLife;
	weak.lifeSpan = 30;
	weak.playerNr = 1;
	weak.hitPoints = 1;
	weak.mass = 1;
	weak.mask = &m;
	weak.current = weak.next = Vec2i{4200, 4000};
	weak.velocity.setComponents(-worldToVelocity(40), 0);
	weak.onCollision = weaponCollision;
	const EntityId iw = b.spawnBack(std::move(weak));

	// They close at 80 a frame across 400 units: contact by frame 6.
	for (int i = 0; i < 8; ++i)
		b.step();

	CHECK(b.get(iw) == nullptr, "the weak shot should be destroyed");
	CHECK(b.get(it) != nullptr, "and the tough one should still be flying");
	if (b.get(it) == nullptr)
		return;
	CHECK(b.get(it)->hitPoints == 2,
			"having paid the weak shot's damage from its hit points, got %ld",
			static_cast<long>(b.get(it)->hitPoints));
	// They meet near x=4000 at frame 5; eight frames of unimpeded flight from
	// 3800 is 4120 -- well past the impact point, with no truncation.
	CHECK(b.get(it)->current.x == 3800 + 8 * 40,
			"and carried on past the impact point unimpeded, got %ld",
			static_cast<long>(b.get(it)->current.x));
}

void
testTurningIntoOverlapIsReverted()
{
	// The overlap-repair protocol (process.c:453-506): when a silhouette
	// CHANGE creates a standing overlap -- a ship rotating against a wall --
	// the C reverts the frame and the facing rather than resolving a
	// collision. You cannot rotate into a planet.
	static std::array<CollisionMask, 2> masks = [] {
		return std::array<CollisionMask, 2>{solid(4, 4), solid(16, 16)};
	}();
	static ShipSpec d = [] {
		ShipSpec d_ = earthlingCruiser();
		d_.turnWait = 0;
		d_.facingMasks = std::span<const CollisionMask>(masks);
		return d_;
	}();
	static const CollisionMask wall = solid(16, 16);

	Battle b(1);

	Element planet;
	planet.kind = ElementKind::Planet;
	planet.playerNr = -1;
	planet.mass = 200;
	planet.hitPoints = 200;
	planet.lifeSpan = 2;
	planet.mask = &wall;
	planet.current = planet.next = Vec2i{4000, 4000};
	planet.onCollision = solidCollision;
	(void)b.spawnBack(std::move(planet));

	// Adjacent at facing 0 (a 4x4 mask), overlapping at facing 1 (16x16).
	const EntityId ship = addShip(b, d, Vec2i{4052, 4000}, 0, 0);
	b.get(ship)->preProcess = shipPreProcess;
	b.get(ship)->postProcess = shipPostProcess;
	b.get(ship)->onCollision = solidCollision;
	b.step();
	CHECK(b.collisions().empty(), "setup: adjacent is not touching");

	const std::int32_t crew = b.get(ship)->ship.crew;
	b.get(ship)->ship.input = ShipInput::Right;
	b.step();

	CHECK(b.get(ship)->facing == 0,
			"the turn into the wall should have been undone, facing %d",
			b.get(ship)->facing);
	CHECK(b.collisions().empty(),
			"and a reverted turn is not a collision");
	CHECK(b.get(ship)->ship.crew == crew,
			"so it costs nothing, got %ld (was %ld)",
			static_cast<long>(b.get(ship)->ship.crew),
			static_cast<long>(crew));
}

int g_overlapDeaths = 0;

void
countOverlapDeath(Battle &, EntityId) noexcept
{
	++g_overlapDeaths;
}

void
testSpawnInsideSomethingIsExecuted()
{
	// The other half of the protocol (process.c:427-449): an APPEARING solid
	// found standing inside another solid dies on the spot, death hook and
	// all -- an asteroid respawned onto the planet becomes rubble at once
	// instead of drifting through it.
	static const CollisionMask m = solid(8, 8);
	Battle b(1);

	Element planet;
	planet.kind = ElementKind::Planet;
	planet.playerNr = -1;
	planet.mass = 200;
	planet.hitPoints = 200;
	planet.lifeSpan = 2;
	planet.mask = &m;
	planet.current = planet.next = Vec2i{4000, 4000};
	planet.onCollision = solidCollision;
	(void)b.spawnBack(std::move(planet));
	b.step();  // established

	Element rock;
	rock.kind = ElementKind::Asteroid;
	rock.playerNr = -1;
	rock.hitPoints = 1;
	rock.mass = 3;
	rock.lifeSpan = 1;
	rock.mask = &m;
	rock.current = rock.next = Vec2i{4004, 4000};  // inside the planet
	rock.onCollision = solidCollision;
	rock.onDeath = countOverlapDeath;
	const EntityId ir = b.spawnBack(std::move(rock));

	g_overlapDeaths = 0;
	b.step();

	CHECK(b.get(ir) == nullptr,
			"a solid spawned inside another is destroyed the same frame");
	CHECK(g_overlapDeaths == 1,
			"with its death hook run, got %d", g_overlapDeaths);
}

void
testPointDefenceBurnsOwnNuke()
{
	// The C's point defence has no ownership filter (human.c:203-204): a
	// Cruiser holding SPECIAL pays for and shoots down its OWN in-flight
	// nuke. That is a real tactical constraint, not a bug -- review-001 A15,
	// decided faithful.
	static CollisionMask shotMask = solid(3, 3);
	static ShipSpec d = [] {
		ShipSpec d_ = earthlingCruiser();
		d_.weapon.masks = std::span<const CollisionMask>(&shotMask, 1);
		return d_;
	}();

	Battle b(1);
	const EntityId ship = addShip(b, d, Vec2i{4000, 4000}, 0, 0);
	b.get(ship)->preProcess = shipPreProcess;
	b.get(ship)->postProcess = shipPostProcess;
	b.step();

	b.get(ship)->ship.input = ShipInput::Weapon;
	b.step();
	std::size_t weapons = 0;
	for (EntityId e = b.elements().front(); e.valid(); e = b.elements().next(e))
		if (b.get(e)->kind == ElementKind::Weapon)
			++weapons;
	CHECK(weapons == 1, "setup: one nuke in flight, got %zu", weapons);

	const std::int32_t energy = b.get(ship)->ship.energy;
	b.get(ship)->ship.input = ShipInput::Special;
	b.step();
	b.get(ship)->ship.input = ShipInput::None;
	b.step();  // the burned nuke's death is seen the following frame

	weapons = 0;
	for (EntityId e = b.elements().front(); e.valid(); e = b.elements().next(e))
		if (b.get(e)->kind == ElementKind::Weapon)
			++weapons;
	CHECK(weapons == 0, "the Cruiser's own nuke should be shot down, %zu left",
			weapons);
	CHECK(b.get(ship)->ship.energy == energy - 4,
			"and the laser paid for (special cost 4), got %ld (was %ld)",
			static_cast<long>(b.get(ship)->ship.energy),
			static_cast<long>(energy));
}

void
testCommittedElementsAreNotIntegratedTwice()
{
	// The C's POST_PROCESS flag protects a committed element from the
	// whole-list catch-up walks (process.c:859). Without that protection, a
	// ship firing every frame -- whose weapon triggers a catch-up from the
	// head every post pass -- is preprocessed twice a frame: double motion,
	// double turning, double energy clocks.
	Battle b(1);
	const EntityId id = addShip(b, ilwrathAvenger(), Vec2i{4000, 6000}, 0, 0);
	b.get(id)->preProcess = shipPreProcess;
	b.get(id)->postProcess = shipPostProcess;
	b.step();

	// Turn and fire together. The Avenger turns every turnWait+1 = 3 frames:
	// frames 1, 4, 7, 10 -- four steps in twelve. A double-preprocessed ship
	// turns visibly faster.
	const int start = b.get(id)->facing;
	b.get(id)->ship.input = ShipInput::Right | ShipInput::Weapon;
	for (int i = 0; i < 12; ++i)
		b.step();

	CHECK(b.get(id)->facing == normalizeFacing(start + 4),
			"twelve frames of turn-and-fire should turn exactly 4 facings, "
			"got %d from %d", b.get(id)->facing, start);
}

void
testDefyPhysicsExpires()
{
	// DEFY_PHYSICS lasts from a collision to the next frame without one
	// (process.c:824-829). Held forever it would permanently disable the
	// post-collision control stagger and steer later stationary contacts into
	// the zero-velocity branch meant for genuinely stuck pairs.
	Battle b(1);
	Element e;
	e.flags = ElementFlags::DefyPhysics;
	const EntityId id = b.spawnBack(std::move(e));
	b.step();
	CHECK(!any(b.get(id)->flags & ElementFlags::DefyPhysics),
			"a frame without a collision sheds DefyPhysics");
}

void
testPointDefenceBurnsIncomingFire()
{
	const CollisionMask m = solid(8, 8);
	Battle b(1);

	const EntityId ship =
			addShip(b, earthlingCruiser(), Vec2i{4000, 4000}, 0, 0);
	b.get(ship)->preProcess = shipPreProcess;
	b.get(ship)->postProcess = shipPostProcess;
	b.get(ship)->mask = &m;
	b.step();

	// An enemy shot well inside LASER_RANGE, which is 100 display pixels --
	// 400 world units.
	Element shot;
	shot.kind = ElementKind::Weapon;
	shot.playerNr = 1;
	shot.flags = ElementFlags::FiniteLife;
	shot.lifeSpan = 20;
	shot.hitPoints = 1;
	shot.damage = 1;
	shot.mass = 1;
	shot.mask = &m;
	shot.current = shot.next = Vec2i{4000, 4200};
	const EntityId incoming = b.spawnBack(std::move(shot));

	b.get(ship)->ship.input = ShipInput::Special;
	b.step();

	CHECK(b.get(incoming) == nullptr || b.get(incoming)->hitPoints == 0,
			"point defence should have burned the incoming shot");
	CHECK(b.get(ship)->ship.specialCounter > 0, "and started its cooldown");

	// The beam is a real element, so the renderer has nothing to invent and
	// a replay draws exactly what the original did.
	int beams = 0;
	for (EntityId e = b.elements().front(); e.valid(); e = b.elements().next(e))
		if (b.get(e)->kind == ElementKind::Laser)
			++beams;
	CHECK(beams == 1, "and left a beam to draw, got %d", beams);
}

void
testShipWarpsInBeforeItIsSolid()
{
	// The arrival, checked without a window.
	//
	// This exists because the effect shipped once already and could not be
	// seen: it was written as a stack of points on a stationary hull, which is
	// invisible against the hull. Watching for it by screenshot means racing a
	// 15-frame window against process start, which is why it went unverified.
	// Stepping the battle asks the question directly.
	sim::Battle b{7u};

	// A hull to be shaped like. Headless, so there is no sprite to take a
	// real silhouette from, but the shadow only has to *carry* it.
	static const sim::CollisionMask hull = solid(12, 12);

	sim::Element e;
	e.kind = sim::ElementKind::Ship;
	e.flags = sim::ElementFlags::PlayerShip;
	e.mask = &hull;
	e.current = Vec2i{4000, 4000};
	e.next = e.current;
	e.facing = 4;
	e.playerNr = 0;
	e.ship.spec = &sim::earthlingCruiser();
	e.mass = e.ship.spec->mass;
	e.preProcess = sim::shipTransition;
	b.spawnBack(std::move(e));

	const auto ship = [&b]() -> const sim::Element * {
		for (sim::EntityId id = b.elements().front(); id.valid();
				id = b.elements().next(id))
		{
			auto p = b.get(id);
			if (p != nullptr && p->kind == sim::ElementKind::Ship)
				return p.raw();
		}
		return nullptr;
	};
	const auto shadows = [&b]() {
		int n = 0;
		for (sim::EntityId id = b.elements().front(); id.valid();
				id = b.elements().next(id))
		{
			auto p = b.get(id);
			if (p != nullptr && p->kind == sim::ElementKind::ShipShadow)
				++n;
		}
		return n;
	};

	b.step();
	CHECK(ship() != nullptr, "the ship should survive its first frame");
	CHECK(any(ship()->flags & sim::ElementFlags::NonSolid),
			"an arriving ship must be intangible -- that is what stops two of "
			"them materialising inside each other");

	// Partway through: shadows are being shed, they are hull-sized rather than
	// points, and -- the part that was wrong twice -- each new one is laid
	// *closer* to the arrival point than the last, so the trail converges onto
	// the ship instead of streaming away from it.
	const Vec2i arrival = ship()->current;
	const auto newestDistance = [&b, &arrival]() -> std::int64_t {
		std::int32_t best = -1;
		std::int64_t dist = -1;
		for (sim::EntityId id = b.elements().front(); id.valid();
				id = b.elements().next(id))
		{
			auto p = b.get(id);
			if (p == nullptr || p->kind != sim::ElementKind::ShipShadow)
				continue;
			if (p->lifeSpan <= best)
				continue;
			best = p->lifeSpan;
			const Vec2i d = sim::wrapDelta(Vec2i{p->current.x - arrival.x,
					p->current.y - arrival.y});
			dist = static_cast<std::int64_t>(d.x) * d.x
					+ static_cast<std::int64_t>(d.y) * d.y;
		}
		return dist;
	};

	int peak = 0;
	int closing = 0;
	int receding = 0;
	std::int64_t previous = -1;
	for (int i = 0; i < sim::kWarpInFrames - 2; ++i)
	{
		b.step();
		peak = std::max(peak, shadows());

		const std::int64_t d = newestDistance();
		if (d >= 0 && previous >= 0)
		{
			if (d < previous)
				++closing;
			else if (d > previous)
				++receding;
		}
		if (d >= 0)
			previous = d;
	}
	CHECK(peak > 0, "a warping ship should shed shadows, saw none");
	CHECK(closing > 0 && receding == 0,
			"the trail must close onto the arrival point, not stream away "
			"from it -- %d frames closed, %d receded",
			closing, receding);

	const sim::Element *s = nullptr;
	for (sim::EntityId id = b.elements().front(); id.valid();
			id = b.elements().next(id))
	{
		auto p = b.get(id);
		if (p != nullptr && p->kind == sim::ElementKind::ShipShadow)
		{
			s = p.raw();
			break;
		}
	}
	if (s != nullptr)
	{
		CHECK(s->mask == &hull,
				"a shadow carries the hull's mask, or it is drawn at a "
				"fallback size and reads as debris rather than as the ship");
		CHECK(s->facing == 4, "a shadow keeps the facing it was shed at");

		// And it lies *behind* the ship. The exhaust's own direction is the
		// reference: spawnIonTrail offsets along facingToAngle + kHalfCircle
		// and that trail is known to come out of the engines, so the same
		// vector negated is forward.
		const int ahead = sim::facingToAngle(4);
		const Vec2i fwd{sim::cosine(ahead, 1000), sim::sine(ahead, 1000)};
		const Vec2i off = sim::wrapDelta(
				Vec2i{s->current.x - arrival.x, s->current.y - arrival.y});
		const std::int64_t dot = static_cast<std::int64_t>(off.x) * fwd.x
				+ static_cast<std::int64_t>(off.y) * fwd.y;
		CHECK(dot < 0,
				"the trail must lie behind the ship, not ahead of it "
				"(dot=%lld, offset=%d,%d forward=%d,%d)",
				static_cast<long long>(dot), off.x, off.y, fwd.x, fwd.y);
	}

	// And it arrives: solid, still, and driving itself from here.
	for (int i = 0; i < 4; ++i)
		b.step();
	CHECK(ship() != nullptr, "the ship should still be here once it arrives");
	CHECK(!any(ship()->flags & sim::ElementFlags::NonSolid),
			"an arrived ship must be solid");
	CHECK(ship()->preProcess == sim::shipPreProcess,
			"an arrived ship drives itself");
	CHECK(ship()->ship.crew == sim::earthlingCruiser().maxCrew,
			"arriving fills the crew, got %d", ship()->ship.crew);
}

void
testCloakHidesFromTracking()
{
	Battle b(1);
	const EntityId avenger =
			addShip(b, ilwrathAvenger(), Vec2i{4000, 4000}, 0, 1);
	b.get(avenger)->preProcess = shipPreProcess;
	b.get(avenger)->postProcess = shipPostProcess;
	b.step();

	const EntityId hunter =
			addShip(b, earthlingCruiser(), Vec2i{4000, 4400}, 0, 0);
	b.get(hunter)->preProcess = shipPreProcess;
	b.step();

	// Facing 8 is away from the target, so a step toward it is a real change.
	// Starting already pointed at it returns 0 quite correctly -- there is
	// nothing to turn -- which is not the same as "no target found".
	int facing = 8;
	CHECK(trackShip(b, hunter, facing) != 0,
			"an uncloaked enemy should be trackable");

	b.get(avenger)->ship.input = ShipInput::Special;
	b.step();
	b.get(avenger)->ship.input = ShipInput::None;

	// Activation starts the colour walk at white; the ship is not hidden
	// yet. OBJECT_CLOAKED is STAMPFILL *and* BLACK (element.h:201-204), so
	// the whole five-colour fade is still targetable -- being missile-proof
	// from the frame SPECIAL lands would be a sizeable unearned buff.
	CHECK(!any(b.get(avenger)->flags & ElementFlags::Cloaked),
			"activation alone must not hide the ship");
	int fadeFacing = 8;
	CHECK(trackShip(b, hunter, fadeFacing) >= 0,
			"a half-faded ship is still targetable");

	// Five walk steps later it is black, and hidden.
	for (int i = 0; i < 5; ++i)
		b.step();
	CHECK(any(b.get(avenger)->flags & ElementFlags::Cloaked),
			"fully faded should be cloaked");

	int cloakedFacing = 8;
	CHECK(trackShip(b, hunter, cloakedFacing) < 0,
			"a cloaked ship must not be targetable at all -- TrackShip "
			"returns -1, no target (weapon.c:344-348, 410-412)");

	// It does *not* lift on its own. This test used to assert the opposite,
	// and the assertion was wrong: the cloak was being driven off
	// specialCounter, so it expired after the 13 frames of SPECIAL_WAIT.
	// ilwrath.c:251-253 only unwinds the ramp when SPECIAL is pressed again
	// or the hull is not yet fully black, so a ship left alone stays hidden.
	b.get(avenger)->ship.input = ShipInput::None;
	for (int i = 0; i < 40; ++i)
		b.step();
	CHECK(any(b.get(avenger)->flags & ElementFlags::Cloaked),
			"a cloak stays on until it is switched off, not until a timer "
			"runs out");

	// A second press drops it.
	b.get(avenger)->ship.input = ShipInput::Special;
	b.step();
	b.get(avenger)->ship.input = ShipInput::None;
	for (int i = 0; i < 20; ++i)
		b.step();
	CHECK(!any(b.get(avenger)->flags & ElementFlags::Cloaked),
			"a second press should uncloak it (ilwrath.c:251-253)");

	// And firing gives you away, permanently -- the ramp runs all the way
	// back even after the trigger is released (ilwrath.c:249-252).
	b.get(avenger)->ship.input = ShipInput::Special;
	b.step();
	b.get(avenger)->ship.input = ShipInput::None;
	for (int i = 0; i < 20; ++i)
		b.step();
	CHECK(any(b.get(avenger)->flags & ElementFlags::Cloaked),
			"it should be hidden again before the firing check");

	b.get(avenger)->ship.input = ShipInput::Weapon;
	b.step();
	b.get(avenger)->ship.input = ShipInput::None;
	for (int i = 0; i < 20; ++i)
		b.step();
	CHECK(!any(b.get(avenger)->flags & ElementFlags::Cloaked),
			"firing should drop the cloak and it should not come back on its "
			"own");
}

void
testCloakedFiringSnapAims()
{
	Battle b(1);
	const EntityId avenger =
			addShip(b, ilwrathAvenger(), Vec2i{4000, 4000}, 0, 1);
	b.get(avenger)->preProcess = shipPreProcess;
	b.get(avenger)->postProcess = shipPostProcess;
	b.step();

	const EntityId hunter =
			addShip(b, earthlingCruiser(), Vec2i{4400, 4000}, 0, 0);
	b.get(hunter)->preProcess = shipPreProcess;
	b.step();

	// Cloak fully: activation plus the five-colour walk.
	b.get(avenger)->ship.input = ShipInput::Special;
	b.step();
	b.get(avenger)->ship.input = ShipInput::None;
	for (int i = 0; i < 5; ++i)
		b.step();
	CHECK(any(b.get(avenger)->flags & ElementFlags::Cloaked),
			"setup: the Avenger should be hidden");

	// Point it the wrong way, then fire from the dark.
	b.get(avenger)->facing = 8;
	b.get(avenger)->ship.input = ShipInput::Weapon;
	b.step();

	// The ambush snap (ilwrath.c:281-342): the discharge aims the ship at
	// the lead-predicted target before the shot leaves. Both ships are at
	// rest here, so the prediction is the target itself -- due +x, facing 4.
	CHECK(b.get(avenger)->facing == 4,
			"firing from full black should snap the facing onto the target, "
			"got %d", b.get(avenger)->facing);
	CHECK(!any(b.get(avenger)->flags & ElementFlags::Cloaked),
			"and the discharge steps the cloak off black");
	CHECK(b.get(avenger)->ship.specialCounter == 0,
			"and zeroes the special debounce, so re-cloak is immediate once "
			"solid (ilwrath.c:347)");
}

}  // namespace

int
main()
{
	testPointDefenceBurnsIncomingFire();
	testCloakHidesFromTracking();
	testCloakedFiringSnapAims();
	testDeltaCrewReportsDeathOnTheExactHit();
	testPlanetsTakeNoDamage();
	testMissileDamagesAndSpendsItself();
	testFlyingIntoAPlanetCostsCrewOverFour();
	testOverlappingShipsSeparateInsteadOfSticking();
	testShipShotMidFlightKeepsItsMotion();
	testToughWeaponPiercesWeakOne();
	testTurningIntoOverlapIsReverted();
	testSpawnInsideSomethingIsExecuted();
	testPointDefenceBurnsOwnNuke();
	testCommittedElementsAreNotIntegratedTwice();
	testDefyPhysicsExpires();
	testGravityMassThreshold();
	testGravityPullsTowardTheSource();
	testGravityHasAHardEdge();
	testFleeingShipIsImmuneToGravity();
	testAsteroidsSpawnOnAnEdgeAndRepeatably();
	testAsteroidTumbles();
	testDestroyedAsteroidIsReplaced();
	testPlanetPlacementAvoidsEverything();
	testShipInitialisesFromItsDescriptor();
	testTurningIsGatedByTurnWait();
	testFiringSpendsEnergyAndRespectsCooldown();
	testMissileFliesAndExpires();
	testFiringPostponesEnergyRegen();
	testSpecialFiresTheFrameItsCounterExpires();
	testOpposingMissilesDestroyEachOther();
	testTheTwoShipsFeelDifferent();
	testIsqrtIsFloorSqrt();
	testHeadOnCollisionExchangesMomentum();
	testGravityMassIsNotPushed();
	testStuckPairIsWorkedApart();
	testDeriveSpeedStateFromVelocity();
	testStepVisitsInListOrder();
	testMidFrameSpawnIsCaughtUpSameFrame();
	testFiniteLifeExpiresAndCallsDeath();
	testMotionIntegratesAndWraps();
	testCollisionPairsAreVisitedOnce();
	testSpawningIsRepeatable();
	testSpawnCarriesTheShipsParameters();
	testTheTwoShipsDifferWhereTheCDoes();
	testThrustTakesItsFacingAsAnArgument();
	testThrustReachesAndHoldsMaxSpeed();
	testInertialessThrustIsInstant();
	testGravityWellAllowsExceedingMax();
	testVelocityCarriesSubUnitDrift();
	testVelocityNegativeEncoding();
	testVelocityAngles();
	testCollisionNeedsNoGraphicsContext();
	testCollisionBasics();
	testCollisionIsPerPixelNotBoxes();
	testCollisionEdgeCases();
	testTrigRoundTrips();
	testArctanSentinel();
	testWorldWrapping();
	testStreamsAreIndependent();
	testReseedReturnsThePrevious();
	testTruncationWidthMatters();
	testTraversalOrderIsWhatWasAskedFor();
	testSlotReuseDoesNotReorder();
	testStaleHandlesAreDetectable();
	testRemovalDuringTraversal();
	testEntityAddressesAreStable();
	testShipWarpsInBeforeItIsSolid();

	if (failures != 0)
		std::printf("%d check(s) failed\n", failures);
	else
		std::printf("sim: golden RNG vectors checked at compile time; "
					"runtime checks passed\n");
	return failures != 0 ? 1 : 0;
}
