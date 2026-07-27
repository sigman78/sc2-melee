// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.
//
// sim/ tests. Much of what matters here is asserted at compile time -- the
// RNG's golden vectors are `static_assert`s, so this file merely has to
// compile for them to have been checked. What is left is the behaviour that
// needs a running object: stream independence and reseed semantics.

#include "engine/core/Pacing.hpp"
#include "sim/Battle.hpp"
#include "sim/Collision.hpp"
#include "sim/EntityList.hpp"
#include "sim/Impulse.hpp"
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

using namespace uqm;       // Vec2i, Pacer, Ticks
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
// Pacing

// The time of display frame `f` at `fps`. Computed from the frame number
// rather than by accumulating a per-frame delta, because 840 does not divide
// evenly by every refresh rate -- 840/144 truncates to 5 and a clock built
// out of those runs 14% slow. The display rate is not required to divide the
// tick rate, and the Pacer has to hold 24 Hz when it does not.
constexpr uqm::Ticks
frameTime(int f, int fps)
{
	return uqm::kOneSecond * f / fps;
}

// Runs `seconds` of display frames at `fps` and counts simulation steps, the
// way the real loop would.
constexpr int
runPacer(uqm::Pacer &pacer, int fps, int seconds)
{
	int steps = 0;
	for (int f = 1; f <= fps * seconds; ++f)
		steps += pacer.stepsDue(frameTime(f, fps));
	return steps;
}

// The naive form the plan warns about: deadline reset to `now` rather than
// advanced by a period. Reproduced here so the test can show what it costs.
constexpr int
runNaive(uqm::Ticks period, int fps, int seconds)
{
	int steps = 0;
	uqm::Ticks next = 0;
	for (int f = 1; f <= fps * seconds; ++f)
	{
		const uqm::Ticks now = frameTime(f, fps);
		if (now >= next + period)
		{
			++steps;
			next = now;
		}
	}
	return steps;
}

void
testPacingHitsTwentyFourHertz()
{
	using namespace uqm;

	// A 60 Hz display loop over ten seconds. 840/60 is 14 ticks a frame.
	Pacer pacer;
	const int steps = runPacer(pacer, 60, 10);
	CHECK(steps == kBattleHz * 10,
			"24 Hz sim under a 60 Hz loop should be %lld steps in 10s, got %d",
			static_cast<long long>(kBattleHz * 10), steps);

	// And the bug, measured: the naive form cannot fire until a whole period
	// has passed since the last step, so it lands on every third 14-tick
	// frame -- 42 ticks, 20 Hz -- for a 17% slowdown.
	const int naive = runNaive(kBattleFrameRate, 60, 10);
	CHECK(naive == 20 * 10,
			"the naive deadline reset should give 20 Hz, got %d", naive);
	CHECK(naive < steps, "the naive form must be measurably slower");

	// A refresh rate that does not divide 840, and one slower than the sim.
	Pacer fast;
	const int fastSteps = runPacer(fast, 144, 10);
	CHECK(fastSteps == kBattleHz * 10,
			"24 Hz should survive a 144 Hz display loop, got %d", fastSteps);

	Pacer slow;
	const int slowSteps = runPacer(slow, 30, 10);
	CHECK(slowSteps == kBattleHz * 10,
			"24 Hz should survive a 30 Hz display loop, got %d", slowSteps);
}

void
testPacingBoundsCatchUp()
{
	using namespace uqm;

	// A backgrounded tab hands back an hour. Catching that up would run
	// 86,400 steps and hang; the cap drops the backlog instead.
	Pacer pacer(kBattleFrameRate, 5);
	const int steps = pacer.stepsDue(kOneSecond * 3600);
	CHECK(steps == 5, "catch-up should be capped at 5, got %d", steps);

	// ...and the cadence resumes from there rather than owing an hour.
	const Ticks resumed = kOneSecond * 3600;
	CHECK(pacer.stepsDue(resumed) == 0, "no backlog should survive the cap");
	CHECK(pacer.stepsDue(resumed + kBattleFrameRate) == 1,
			"the next step should be one period after the resync");
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
	v.weaponSpeed = 24;       // MISSILE_SPEED == MAX_THRUST (human.c:31, 44)
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
	const Element *e = b.get(id);
	g_trace.preOrder.push_back(static_cast<int>(e->mass));
}

void
recordDeath(Battle &b, EntityId id) noexcept
{
	const Element *e = b.get(id);
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
	child.flags = ElementFlags::FiniteLife;
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
	shot.flags = ElementFlags::FiniteLife;
	shot.lifeSpan = 3;
	shot.onDeath = recordDeath;
	b.spawnBack(std::move(shot));

	b.step();
	CHECK(b.elements().size() == 1, "alive after one step");
	b.step();
	b.step();
	CHECK(b.elements().empty(), "a 3-frame life should be gone after 3 steps");
	CHECK(g_trace.deaths == std::vector<int>({7}),
			"and its death hook should have run exactly once");
}

void
testMotionIntegratesAndWraps()
{
	Battle b(1);
	Element e;
	e.current = Vec2i{100, 100};
	e.velocity.setComponents(worldToVelocity(10), 0);
	const EntityId id = b.spawnBack(std::move(e));

	// The first step only seeds `next` -- an Appearing element has a position
	// but no motion yet, matching process.c:379.
	b.step();
	CHECK(b.get(id)->current == Vec2i({100, 100}),
			"a newly spawned element does not move on its first step");

	b.step();
	CHECK(b.get(id)->current.x == 110, "then it moves 10 a frame, got %ld",
			static_cast<long>(b.get(id)->current.x));

	// And the arena is a torus.
	b.get(id)->current = Vec2i{kLogSpaceWidth - 5, 0};
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
	const EntityId ia = b.spawnBack(std::move(a));

	Element c;
	c.current = Vec2i{500, 500};
	c.mask = &mask;
	c.kind = ElementKind::Ship;
	c.playerNr = 1;
	const EntityId ic = b.spawnBack(std::move(c));

	b.step();
	CHECK(b.get(ia)->collidedWith == ic, "a should record hitting c");
	CHECK(b.get(ic)->collidedWith == ia, "and c should record hitting a");

	// Two projectiles from the same owner with IgnoreSimilar must not hit
	// each other -- ilwrath.c:203, a flame stream not eating itself.
	Battle b2(1);
	Element f1;
	f1.current = Vec2i{500, 500};
	f1.mask = &mask;
	f1.kind = ElementKind::Weapon;
	f1.playerNr = 0;
	f1.flags = ElementFlags::IgnoreSimilar;
	const EntityId if1 = b2.spawnBack(std::move(f1));

	Element f2 = *b2.get(if1);
	f2.flags = ElementFlags::IgnoreSimilar;
	const EntityId if2 = b2.spawnBack(std::move(f2));

	b2.step();
	CHECK(!b2.get(if1)->collidedWith.valid(),
			"same-owner flames must not collide");
	CHECK(!b2.get(if2)->collidedWith.valid(), "either way round");
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

}  // namespace

int
main()
{
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
	testPacingHitsTwentyFourHertz();
	testPacingBoundsCatchUp();
	testTraversalOrderIsWhatWasAskedFor();
	testSlotReuseDoesNotReorder();
	testStaleHandlesAreDetectable();
	testRemovalDuringTraversal();

	if (failures != 0)
		std::printf("%d check(s) failed\n", failures);
	else
		std::printf("sim: golden RNG vectors checked at compile time; "
					"runtime checks passed\n");
	return failures != 0 ? 1 : 0;
}
