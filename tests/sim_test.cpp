// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.
//
// sim/ tests. Much of what matters here is asserted at compile time -- the
// RNG's golden vectors are `static_assert`s, so this file merely has to
// compile for them to have been checked. What is left is the behaviour that
// needs a running object: stream independence and reseed semantics.

#include "engine/core/Types.hpp"
#include "sim/Battle.hpp"
#include "sim/Collision.hpp"
#include "sim/Damage.hpp"
#include "sim/Field.hpp"
#include "sim/Gravity.hpp"
#include "sim/Impulse.hpp"
#include "sim/Random.hpp"
#include "sim/Ship.hpp"
#include "sim/ShipSystems.hpp"
#include "sim/Spawn.hpp"
#include "sim/Targeting.hpp"
#include "sim/Thrust.hpp"
#include "sim/Trig.hpp"
#include "sim/Velocity.hpp"
#include "sim/World.hpp"
#include "sim/ships/Human.hpp"
#include "sim/ships/Ilwrath.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace uqm;       // Vec2i
using namespace uqm::sim;  // Rng, Battle, the world constants

namespace {

int failures = 0;

#define CHECK(cond, ...)                                                       \
	do                                                                         \
	{                                                                          \
		if (!(cond))                                                           \
		{                                                                      \
			std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                   \
			std::printf(__VA_ARGS__);                                          \
			std::printf("\n");                                                 \
			++failures;                                                        \
		}                                                                      \
	} while (0)

void testStreamsAreIndependent()
{
	// Presentation must draw from its own stream so it cannot perturb the sim
	// (commanim.c draws from the sim's, on a wall-clock schedule): same seed
	// agrees, but drawing from one stream must not move the other.
	Rng sim(999);
	Rng presentation(999);
	CHECK(sim.next() == presentation.next(), "same seed, same first draw");

	const u32 before = sim.seed();
	(void)presentation.next();
	(void)presentation.next();
	CHECK(sim.seed() == before,
			"drawing from one stream must not move another");
}

void testReseedReturnsThePrevious()
{
	// TFB_SeedRandom returns the old seed; the C uses that to save and
	// restore a stream around a private one (melnorm.c's getStripRandomSeed).
	Rng rng(4321);
	const u32 saved = rng.reseed(1000);
	CHECK(saved == 4321, "reseed returns the previous seed, got %u", saved);
	CHECK(rng.seed() == 1000, "reseed installs the new seed");

	const u32 a = rng.next();
	rng.reseed(1000);
	CHECK(rng.next() == a, "restoring a seed replays the stream");
}

void testTruncationWidthMatters()
{
	// Call sites truncate to 16 bits before taking a modulo -- `(COUNT)`,
	// `(UWORD)`, `LOWORD` -- and that changes the answer. If these ever
	// agreed, one of the two is wrong.
	Rng wide(12345);
	Rng narrow(12345);
	const u32 full = wide.next() % 100u;
	const u32 trunc = narrow.next16() % 100u;
	CHECK(full != trunc,
			"16-bit truncation must change the result for this seed "
			"(%u vs %u)",
			full, trunc);
}

// --------------------------------------------------------------------------
// Battle storage
//
// Order (Entity.hpp) is the declared sort key eachOrdered reads: layers walk
// in enum order, FIFO within a layer, stable across entt's slot reuse.

// A minimal, marked spawn for the tests below: no Collider, so it takes no
// part in collision math (spawn() attaches one only when given a mask), no
// hooks, and playerNr stored on Allegiance as a bare label.
EntityId spawnMarked(Battle &b, Layer layer, int playerNr)
{
	return b.spawn(layer, comp::Position{}, comp::Motion{}, comp::Physique{},
			nullptr, comp::Allegiance{playerNr, kNoEntity});
}

// Walks in declared order, collecting each element's label.
std::vector<int> playerNrs(Battle &b)
{
	std::vector<int> out;
	b.eachOrdered([&](EntityId id) {
		out.push_back(b.reg.try_get<comp::Allegiance>(id)->playerNr);
	});
	return out;
}

void testTraversalOrderIsDeclared()
{
	// Order is a declared stratum, not insertion position: layers traverse
	// in enum order, FIFO within a layer.
	Battle b(1);
	spawnMarked(b, Layer::Field, 1);
	spawnMarked(b, Layer::Field, 2);
	CHECK(playerNrs(b) == std::vector<int>({1, 2}),
			"a layer is FIFO within itself");

	// The pkunk.c:498-512 case, declared: Background walks before Field,
	// so a phoenix-like element preprocesses before the field it precedes.
	spawnMarked(b, Layer::Background, 0);
	CHECK(playerNrs(b) == std::vector<int>({0, 1, 2}),
			"an earlier layer walks first, whenever it was spawned");

	// Ordnance after the field -- the C's tail PutElement.
	spawnMarked(b, Layer::Ordnance, 9);
	CHECK(playerNrs(b) == std::vector<int>({0, 1, 2, 9}),
			"a later layer walks last");

	// And a Field spawn now lands BETWEEN the strata: after the field's
	// tail, before the ordnance -- the position insertion order could only
	// express by accident, declared here on purpose.
	spawnMarked(b, Layer::Field, 3);
	CHECK(playerNrs(b) == std::vector<int>({0, 1, 2, 3, 9}),
			"a spawn joins its own stratum, not the global tail");
}

void testSlotReuseDoesNotReorder()
{
	// Storage order and traversal order can disagree once entt hands a
	// slot back. Order (layer, seq), assigned at spawn, keeps a new entity
	// where it was asked to go, not where its recycled slot sits.
	Battle b(1);
	spawnMarked(b, Layer::Field, 1);
	const EntityId second = spawnMarked(b, Layer::Field, 2);
	spawnMarked(b, Layer::Field, 3);

	b.reg.emplace<comp::Lifetime>(second, comp::Lifetime{0});
	b.step();  // reaps the middle entry, freeing its slot for reuse

	spawnMarked(b, Layer::Field, 4);  // which this respawn reuses
	CHECK(playerNrs(b) == std::vector<int>({1, 3, 4}),
			"a reused slot must not drag the new entity back to the old "
			"position");
	CHECK(b.size() == 3,
			"size should track the reap and the respawn, "
			"got %zu",
			b.size());
}

void testStaleHandlesAreDetectable()
{
	// The slot comes straight back; the generation in the versioned entity
	// id is what stops a stale handle from reading its new tenant.
	Battle b(1);
	const EntityId id = spawnMarked(b, Layer::Field, 7);
	CHECK(b.alive(id), "a live handle resolves");

	b.reg.emplace<comp::Lifetime>(id, comp::Lifetime{0});
	b.step();
	CHECK(!b.alive(id), "a reaped handle is not alive");

	// The slot the reap just freed comes straight back.
	const EntityId reused = spawnMarked(b, Layer::Field, 8);
	CHECK(reused != id,
			"the test needs a fresh handle, even if the slot "
			"is the same one");
	CHECK(!b.alive(id), "the stale handle must not resolve to the new one");
	CHECK(b.alive(reused)
					&& b.reg.try_get<comp::Allegiance>(reused)->playerNr == 8,
			"the new handle works");

	CHECK(!b.alive(kNoEntity), "a default handle is never alive");
}

void testTheReapKeepsTheWalkIntact()
{
	// The reap -- Battle's only removal, driven by Lifetime/Doomed and run
	// inside step() -- must leave survivors exactly where they were: a
	// projectile expiring mid-frame is the ordinary case that needs this.
	Battle b(1);
	std::vector<EntityId> ids;
	ids.reserve(5);
	for (int i = 0; i < 5; ++i)
		ids.push_back(spawnMarked(b, Layer::Field, i));

	b.reg.emplace<comp::Lifetime>(ids[1], comp::Lifetime{0});
	b.reg.emplace<comp::Lifetime>(ids[3], comp::Lifetime{0});
	b.step();

	CHECK(playerNrs(b) == std::vector<int>({0, 2, 4}),
			"the reap should leave the survivors in their original order");
	CHECK(b.size() == 3, "size should track the reap, got %zu", b.size());
}

void testEntityAddressesAreStable()
{
	// ShipState::in_place_delete (Ship.hpp) keeps a ship's address stable
	// under entt: reap tombstones the slot instead of swap-and-popping a
	// neighbour into it, and growth extends the pool instead of relocating it.
	Battle b(1);
	const EntityId A = spawnMarked(b, Layer::Field, 1);
	// B sits between the two under test, so its reap has somewhere to
	// disturb the layout if in_place_delete were not honoured.
	const EntityId B = spawnMarked(b, Layer::Field, 2);
	const EntityId C = spawnMarked(b, Layer::Field, 3);

	// Empty spec: ShipMachines now walks anything with a ShipState, so the
	// spec pointer has to be real; empty keeps facingMasks empty too, so
	// this stays the Collider-free spawn spawnMarked's own comment promises.
	static const ShipSpec inertSpec{};
	b.attachShip(A, &inertSpec);
	b.attachShip(C, &inertSpec);
	comp::ShipState const *pa = b.ship(A);
	comp::ShipState const *pc = b.ship(C);

	b.reg.emplace<comp::Lifetime>(B, comp::Lifetime{0});
	b.step();  // reaps B -- a tombstone, not a compaction

	for (int i = 0; i < 100; ++i)
		spawnMarked(b, Layer::Field, 100 + i);  // forces the pool to grow

	CHECK(b.ship(A) == pa, "A's address must survive B's reap and the growth");
	CHECK(b.ship(C) == pc, "and so must C's");
	CHECK(b.reg.try_get<comp::Allegiance>(A)->playerNr == 1,
			"and A's data must still be A's");
	CHECK(b.reg.try_get<comp::Allegiance>(C)->playerNr == 3,
			"and C's still C's");
}

// --------------------------------------------------------------------------
// World topology

void testWorldWrapping()
{
	// The arena the C computes today, now fixed at compile time.
	static_assert(kArena.w == 8192 && kArena.h == 7680);

	static_assert(wrapX(0) == 0);
	static_assert(wrapX(kArena.w) == 0, "the far edge is the near edge");
	static_assert(wrapX(-1) == kArena.w - 1, "stepping off the left");
	static_assert(wrapY(-1) == kArena.h - 1);
	static_assert(wrap(Vec2i{-1, -1}) == Vec2i{kArena.w - 1, kArena.h - 1});

	// The shorter way round. Two entities either side of the seam are close,
	// and an AI that measures the long way flies away from its target.
	static_assert(wrapDeltaX(1) == 1);
	static_assert(wrapDeltaX(kArena.w - 1) == -1,
			"just short of a full lap is one unit backwards");
	static_assert(wrapDeltaX(-(kArena.w - 1)) == 1);
	static_assert(wrapDeltaX(kArena.w / 2) == kArena.w / 2,
			"exactly half stays positive, as WRAP_DELTA_X does");
	static_assert(wrapDeltaY(kArena.h - 1) == -1);

	// Display/world conversion: a pixel is four world units.
	static_assert(displayToWorld(1) == 4);
	static_assert(worldToDisplay(4) == 1);
	static_assert(worldToDisplay(displayToWorld(kSpace.w)) == kSpace.w);
}

// --------------------------------------------------------------------------
// Trig

void testTrigRoundTrips()
{
	// The table's golden values are static_asserts in Trig.hpp; what is worth
	// checking at runtime is that the pieces agree with each other over the
	// whole circle.

	// Every angle's (cos, sin) must arctan back to itself. This is the
	// property the AI leans on -- aim at a target, then read the aim back --
	// and it is where a table transcribed one entry out would show up.
	for (int a = 0; a < kFullCircle; ++a)
	{
		const i32 x = cosine(a, 1000);
		const i32 y = sine(a, 1000);
		const int back = arctan(static_cast<int>(x), static_cast<int>(y));
		CHECK(back == a, "angle %d -> (%ld, %ld) -> %d", a,
				static_cast<long>(x), static_cast<long>(y), back);
	}

	// Magnitude is preserved to within rounding: cos^2 + sin^2 == m^2.
	for (int a = 0; a < kFullCircle; ++a)
	{
		const i64 x = cosine(a, 10000);
		const i64 y = sine(a, 10000);
		const i64 r2 = x * x + y * y;
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
		const i32 here = sine(a, 4096);
		const i32 opposite = sine(a + kHalfCircle, 4096);
		const i32 sum = here + opposite;
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

void testArctanSentinel()
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
CollisionMask solid(u32 w, u32 h)
{
	const std::vector<u8> bits(static_cast<usize>(w) * h, 1);
	return CollisionMask(Extent2u{w, h},
			Vec2i{static_cast<i32>(w / 2), static_cast<i32>(h / 2)}, bits);
}

// A ring: opaque border, hollow middle. Two of these can overlap by bounding
// box while missing entirely, which is the whole point of per-pixel.
CollisionMask ring(u32 w, u32 h)
{
	std::vector<u8> bits(static_cast<usize>(w) * h, 0);
	for (u32 y = 0; y < h; ++y)
		for (u32 x = 0; x < w; ++x)
			if (x == 0 || y == 0 || x == w - 1 || y == h - 1)
				bits[static_cast<usize>(y) * w + x] = 1;
	return CollisionMask(Extent2u{w, h},
			Vec2i{static_cast<i32>(w / 2), static_cast<i32>(h / 2)}, bits);
}

// Whether this frame's collisions() recorded a pair between x and y, order
// either way.
bool pairCollided(const Battle &b, EntityId x, EntityId y) noexcept
{
	for (const CollisionEvent &ev : b.collisions())
		if ((ev.a.id == x && ev.b.id == y) || (ev.a.id == y && ev.b.id == x))
			return true;
	return false;
}

void testCollisionNeedsNoGraphicsContext()
{
	// intersec.c:245 returns "no collision" with no active graphics context,
	// which is why a naive headless build hangs in weapon.c's rejection loop
	// instead of producing wrong numbers. Nothing here has ever seen a
	// renderer.
	const CollisionMask a = solid(4, 4);
	const CollisionMask b = solid(4, 4);
	const Body b0{&a, Vec2i{0, 0}, Vec2i{0, 0}};
	const Body b1{&b, Vec2i{0, 0}, Vec2i{0, 0}};
	CHECK(static_cast<bool>(sweptIntersect(b0, b1)),
			"two overlapping bodies must collide with no context anywhere");
}

void testCollisionBasics()
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
		const i32 gap = hit.at0.x - hit.at1.x;
		CHECK(gap > -8 && gap < 8,
				"impact positions should nearly coincide, "
				"gap %ld",
				static_cast<long>(gap));
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

void testCollisionIsPerPixelNotBoxes()
{
	// Two rings, one small enough to sit inside the other's hollow centre.
	// Their boxes overlap; no opaque pixel does.
	const CollisionMask outer = ring(9, 9);
	const std::vector<u8> dot{1};
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

void testCollisionEdgeCases()
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

void testVelocityCarriesSubUnitDrift()
{
	// Velocity is fixed point with a carried error, not a rounded integer:
	// a drift slower than one world unit per frame still has to move, or a
	// truncating implementation rounds it to zero and it hangs in space
	// forever.
	Velocity v;
	v.setComponents(1, 0);  // 1/32 of a world unit per frame
	CHECK(v.current().x == 1, "a sub-unit component survives being set");

	i32 travelled = 0;
	for (int f = 0; f < 32; ++f)
		travelled += v.advance(1).x;
	CHECK(travelled == 1,
			"1/32 per frame should cover exactly one unit in 32 frames, got "
			"%ld",
			static_cast<long>(travelled));

	// ...and the same total whether taken in one step or many, since the
	// error is carried rather than discarded.
	Velocity bulk;
	bulk.setComponents(1, 0);
	CHECK(bulk.advance(32).x == 1, "one 32-frame step covers the same ground");
}

void testVelocityNegativeEncoding()
{
	// The sign lives in a packed byte, and reconstruction has to recover it
	// exactly -- including the fractional part, which is where the doubled
	// remainder in the high byte earns its keep.
	for (i32 v = -200; v <= 200; ++v)
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
	i32 travelled = 0;
	for (int f = 0; f < 32; ++f)
		travelled += down.advance(1).y;
	CHECK(travelled == -1, "negative sub-unit drift should move -1, got %ld",
			static_cast<long>(travelled));
}

void testVelocityAngles()
{
	// setVector keeps the *facing* as authoritative, so a zero magnitude
	// still remembers which way the object points...
	Velocity aimed;
	aimed.setVector(0, Facing(4));
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

void testThrustTakesItsFacingAsAnArgument()
{
	// The whole point of the primitive: thrusting somewhere other than where
	// the ship points needs no save/overwrite/restore of a global -- Supox's
	// omni-thrust is "pick a facing delta, call thrust", made possible here.
	constexpr ThrustProfile cruiser{24, 3};

	Velocity forward;
	Velocity backward;
	const SpeedState a = thrust(forward, Facing(0), cruiser, ThrustState{});
	const SpeedState b = thrust(backward, Facing(8), cruiser, ThrustState{});
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

void testThrustReachesAndHoldsMaxSpeed()
{
	constexpr ThrustProfile cruiser{24, 3};

	Velocity v;
	ThrustState st;
	int frames = 0;
	while (st.speed == SpeedState::Normal && frames < 100)
	{
		st.speed = thrust(v, Facing(0), cruiser, st);
		++frames;
	}
	CHECK(st.speed == SpeedState::AtMax,
			"a cruiser accelerating in a straight line should reach max");
	CHECK(frames > 1 && frames < 100,
			"and should take several frames to do it, took %d", frames);

	// Once there, further thrust along the same heading is a no-op and the
	// state stays put -- this is the early-out the C takes.
	const Vec2i atMax = v.current();
	st.speed = thrust(v, Facing(0), cruiser, st);
	CHECK(st.speed == SpeedState::AtMax, "still at max");
	CHECK(v.current() == atMax, "and the velocity does not creep upward");
}

void testInertialessThrustIsInstant()
{
	// The Skiff: thrust_increment == max_thrust, so it reaches full speed in
	// one frame and can never be beyond max. The C tests the equality rather
	// than carrying a flag, and so does this.
	constexpr ThrustProfile skiff{40, 40};
	CHECK(skiff.inertialess(), "equal increment and max means inertialess");

	Velocity v;
	const SpeedState s = thrust(v, Facing(4), skiff, ThrustState{});
	CHECK(s == SpeedState::AtMax, "a skiff is at max after one frame");

	// And it turns on a pin: a new facing replaces the vector outright
	// rather than being integrated into it.
	const SpeedState s2 = thrust(v, Facing(12), skiff, ThrustState{s, false});
	CHECK(s2 == SpeedState::AtMax, "still at max after reversing");
	CHECK(v.travelAngle() == facingToAngle(12),
			"and travels the new way immediately");
}

void testGravityWellAllowsExceedingMax()
{
	constexpr ThrustProfile cruiser{24, 3};

	// Get up to the ship's own maximum first.
	Velocity v;
	ThrustState st;
	for (int i = 0; i < 60 && st.speed == SpeedState::Normal; ++i)
		st.speed = thrust(v, Facing(0), cruiser, st);
	CHECK(st.speed == SpeedState::AtMax, "at max before the well");

	// Inside a well, thrust keeps adding speed past the ship's maximum, up to
	// the hard ceiling. That is what a gravity whip is.
	st.inGravityWell = true;
	const Vec2i before = v.current();
	st.speed = thrust(v, Facing(0), cruiser, st);
	CHECK(st.speed == SpeedState::BeyondMax,
			"a well should push the ship beyond its own maximum");
	const Vec2i after = v.current();
	CHECK(after.y < before.y, "and it should actually be faster (%ld -> %ld)",
			static_cast<long>(before.y), static_cast<long>(after.y));
}

// --------------------------------------------------------------------------
// Spawn descriptors

ShipView cruiserView()
{
	ShipView v;
	v.position = Vec2i{1000, 1000};
	v.facing = Facing(3);
	v.playerNr = 0;
	// NOT the real MISSILE_SPEED (40): human.c:41-44 takes
	// max(MAX_THRUST, DISPLAY_TO_WORLD(10)) and the floor wins. This value is
	// arbitrary -- spawn functions pass it through untouched, which this tests.
	v.weaponSpeed = 24;
	v.weaponLife = 60;      // MISSILE_LIFE
	v.weaponDamage = 4;     // MISSILE_DAMAGE
	v.weaponHitPoints = 1;  // MISSILE_HITS
	v.muzzleOffset = 42;    // HUMAN_OFFSET
	v.blastOffset = 8;      // NUKE_OFFSET
	return v;
}

void testSpawningIsRepeatable()
{
	// The property the AI's lookahead needs and the C does not: asking "what
	// would I spawn?" mutates shared state for real in the C (umgah.c:330-341).
	// Here it must be free -- asking a hundred times must change nothing.
	const ShipView ship = cruiserView();

	SpawnBuffer first{};
	const usize n = spawnCruiserPrimary(ship, first);
	CHECK(n == 1, "the cruiser fires one missile, got %zu", n);

	for (int i = 0; i < 100; ++i)
	{
		SpawnBuffer again{};
		const usize m = spawnCruiserPrimary(ship, again);
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

void testSpawnCarriesTheShipsParameters()
{
	const ShipView ship = cruiserView();
	SpawnBuffer buf{};
	const usize n = spawnCruiserPrimary(ship, buf);
	CHECK(n == 1, "one spawn");

	const Spawn &s = buf[0];
	CHECK(s.facing == ship.facing, "the missile inherits the facing");
	CHECK(s.speed == 24 && s.life == 60 && s.damage == 4,
			"and the ship's weapon parameters");
	CHECK(s.playerNr == ship.playerNr, "and its owner");

	// The muzzle sits forward of the hotspot along the facing, not on it.
	CHECK(!(s.position == ship.position),
			"the missile appears at the muzzle, not the hotspot");
	const i32 dx = s.position.x - ship.position.x;
	const i32 dy = s.position.y - ship.position.y;
	const i64 dist2 = i64{dx} * dx + i64{dy} * dy;
	const i64 want = i64{displayToWorld(42)} * displayToWorld(42);
	// Within a few percent: the offset goes through the 14-bit sine table.
	CHECK(dist2 > want * 9 / 10 && dist2 < want * 11 / 10,
			"muzzle should be ~42 display pixels out, got %lld vs %lld",
			static_cast<long long>(dist2), static_cast<long long>(want));
}

void testTheTwoShipsDifferWhereTheCDoes()
{
	ShipView ship = cruiserView();
	ship.facing = Facing(7);

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
	CHECK(!cruiser[0].ignoreSimilar,
			"cruiser missiles collide with each other");
	CHECK(avenger[0].ignoreSimilar, "flame does not collide with itself");
}

// --------------------------------------------------------------------------
// The battle step

// Hooks are free functions, so the things they need to record go here.
struct Trace
{
	std::vector<int> preOrder;
};
Trace g_trace;

// Tags an element so the trace can name it: Physique::mass is unused by
// these tests, so it doubles as a label.
void recordPre(Battle &b, EntityId id) noexcept
{
	g_trace.preOrder.push_back(
			static_cast<int>(b.reg.try_get<comp::Physique>(id)->mass));
}

void testStepVisitsInListOrder()
{
	// eachOrdered is what declares traversal order; this tests it directly
	// against list order and against an earlier layer preempting it.
	g_trace = Trace{};
	Battle b(1);
	b.spawn(Layer::Field, comp::Position{}, comp::Motion{}, comp::Physique{1});
	b.spawn(Layer::Field, comp::Position{}, comp::Motion{}, comp::Physique{2});
	b.spawn(Layer::Field, comp::Position{}, comp::Motion{}, comp::Physique{3});
	b.eachOrdered([&b](EntityId id) { recordPre(b, id); });
	CHECK(g_trace.preOrder == std::vector<int>({1, 2, 3}),
			"eachOrdered should visit in list order");

	// An earlier layer puts the newcomer first -- the pkunk.c phoenix
	// ordering, declared instead of head-inserted.
	b.spawn(Layer::Background, comp::Position{}, comp::Motion{},
			comp::Physique{0});
	g_trace.preOrder.clear();
	b.eachOrdered([&b](EntityId id) { recordPre(b, id); });
	CHECK(g_trace.preOrder == std::vector<int>({0, 1, 2, 3}),
			"an earlier-layer element visits first");
}

void testMidFrameSpawnStaysOutOfItsOwnFrame()
{
	// A spawn built mid-frame is a real entity from the moment its pass
	// asks -- but it holds no Order until the sync point, so no pass that
	// frame reaches it. It is reported as a SpawnEvent for the step it
	// landed in, and takes its first live frame the one after.
	//
	// The vehicle is the asteroid field's own cycle, which emits from
	// ageAndReapMarkPass -- slot 2. AgeDecrement runs at slot 11 of the
	// same step, so the rubble's own countdown is the probe: untouched
	// means it was not in the walk, and there is nowhere for it to hide.
	static const CollisionMask m = solid(8, 8);
	Battle b(1);

	comp::Position rockPos;
	rockPos.current = rockPos.next = Vec2i{100, 100};
	const EntityId rock = b.spawn(
			Layer::Field, rockPos, comp::Motion{}, comp::Physique{3}, &m);
	b.reg.emplace<comp::Lifetime>(rock, comp::Lifetime{1});
	b.reg.emplace<comp::Asteroid>(
			rock, comp::Asteroid{&m, comp::Asteroid::Phase::Solid});

	b.step();  // the rock's own appearing frame: its life ages to 0
	CHECK(b.size() == 1, "setup: only the rock exists so far");

	// This step detects the death, builds the rubble, and lands it.
	b.step();
	CHECK(!b.alive(rock), "the rock dies at the start of this step");
	CHECK(b.spawns().size() == 1, "the rubble is a SpawnEvent this step");

	const EntityId rubble = b.spawns()[0].id;
	CHECK(b.alive(rubble), "and exists by the end of it");
	CHECK(b.size() == 1, "and is in the walk, and counted, once landed");

	// Five frames is what Field.cpp spawns it with. Four would mean
	// AgeDecrement reached it on the frame it was built.
	CHECK(framesLeft(b, rubble) == 5,
			"the rubble should not have aged on its own frame, got %ld",
			static_cast<long>(framesLeft(b, rubble)));

	// It stands where the rock died, and Commit did not publish over it.
	const comp::Position &at = b.reg.get<comp::Position>(rubble);
	const Vec2i wantAt{100, 100};
	CHECK(at.current == wantAt && at.next == at.current,
			"and where the rock died");

	// In the walk now, so the passes reach it.
	b.step();
	CHECK(framesLeft(b, rubble) == 4,
			"and age exactly once the following frame, got %ld",
			static_cast<long>(framesLeft(b, rubble)));
}

// How many rocks stand in each phase of the field's cycle.
[[nodiscard]] int rocksInPhase(Battle &b, comp::Asteroid::Phase want)
{
	int n = 0;
	b.reg.view<comp::Asteroid>().each([&](comp::Asteroid &rock) {
		if (rock.phase == want)
			++n;
	});
	return n;
}

void testFiniteLifeExpiresAndRunsItsDeathResponse()
{
	static const CollisionMask m = solid(8, 8);
	Battle b(1);

	const EntityId id = b.spawn(Layer::Field);
	b.reg.emplace<comp::Lifetime>(id, comp::Lifetime{3});
	b.reg.emplace<comp::Asteroid>(
			id, comp::Asteroid{&m, comp::Asteroid::Phase::Solid});

	// Life is spent in the pre pass and death is decided at the *start* of
	// the frame after it reaches zero (process.c:133-141, 180-181). A
	// 3-frame life therefore survives three steps and dies on the fourth.
	b.step();
	b.step();
	b.step();
	CHECK(b.alive(id), "a 3-frame life survives three steps");
	b.step();
	CHECK(!b.alive(id), "and is gone on the fourth");

	// One rubble, not two: the response runs once, when Doomed is set.
	const int rubble = rocksInPhase(b, comp::Asteroid::Phase::Rubble);
	CHECK(rubble == 1,
			"its death response should have run exactly once, got %d", rubble);
}

void testMotionIntegratesAndWraps()
{
	Battle b(1);
	comp::Motion motion;
	motion.velocity.setComponents(worldToVelocity(10), 0);
	comp::Position pos;
	pos.current = Vec2i{100, 100};
	const EntityId id = b.spawn(Layer::Field, pos, motion);

	// A newly spawned element *does* move on its first step: Appearing
	// suppresses the preprocess hook, not the motion -- process.c:163 gates
	// movement on IGNORE_VELOCITY alone.
	b.step();
	CHECK(b.reg.try_get<comp::Position>(id)->current.x == 110,
			"it should move 10 on its very first step, got %ld",
			static_cast<long>(b.reg.try_get<comp::Position>(id)->current.x));

	b.step();
	CHECK(b.reg.try_get<comp::Position>(id)->current.x == 120,
			"and 10 a frame after, got %ld",
			static_cast<long>(b.reg.try_get<comp::Position>(id)->current.x));

	// The arena is a torus, and teleporting means moving BOTH `current` and
	// `next` (process.c:172-173): integration overwrites a lone `current` at
	// commit, where the wrap itself also happens (process.c:899-916).
	b.reg.try_get<comp::Position>(id)->current = Vec2i{kArena.w - 5, 0};
	b.reg.try_get<comp::Position>(id)->next =
			b.reg.try_get<comp::Position>(id)->current;
	b.step();
	CHECK(b.reg.try_get<comp::Position>(id)->current.x < 100,
			"crossing the seam should wrap, got %ld",
			static_cast<long>(b.reg.try_get<comp::Position>(id)->current.x));
}

void testCollisionPairsAreVisitedOnce()
{
	Battle b(1);
	const std::vector<u8> bits(16, 1);
	static const CollisionMask mask(Extent2u{4, 4}, Vec2i{2, 2}, bits);

	comp::Position aPos;
	aPos.current = Vec2i{500, 500};
	// Transient, like a real weapon: an at-rest overlap between two *solid*
	// bodies is the "BAD NEWS" case the step skips, so only a transient can
	// register this hit. Two frames of life since the first is spent already.
	const EntityId ia = b.spawn(Layer::Field, aPos, comp::Motion{},
			comp::Physique{}, &mask, comp::Allegiance{0, kNoEntity});
	b.reg.emplace<comp::Lifetime>(ia, comp::Lifetime{2});

	// At least one side must have mass, or the pair is skipped entirely
	// (collide.h:39). Two massless things have no momentum to exchange.
	const comp::Physique cPhys{6};
	comp::Position cPos;
	cPos.current = Vec2i{500, 500};
	const EntityId ic = b.spawn(Layer::Field, cPos, comp::Motion{}, cPhys,
			&mask, comp::Allegiance{1, kNoEntity});

	b.step();
	CHECK(pairCollided(b, ia, ic), "a and c should record colliding");

	// Two things from the same *ship* that both carry IgnoreSimilar must not
	// hit each other (collide.h:37-38). Owner, not player: a flame must not
	// burn the Avenger that breathed it.
	Battle b2(1);

	const comp::Physique f1Phys{4};
	comp::Position f1Pos;
	f1Pos.current = Vec2i{500, 500};
	const EntityId if1 = b2.spawn(Layer::Field, f1Pos, comp::Motion{}, f1Phys,
			&mask, comp::Allegiance{0, kNoEntity});
	b2.reg.emplace<comp::IgnoreSimilar>(if1);
	// EntityId{index, generation} isn't a literal you can synthesize -- entt's
	// handle has no such constructor -- so f1 stands in as its own owner, a
	// real spawned id the second projectile below can share.
	b2.reg.try_get<comp::Allegiance>(if1)->owner = if1;

	const comp::Physique f2Phys{7};
	const comp::Position f2Pos = *b2.reg.try_get<comp::Position>(if1);
	// Same owner as f1 -- copied, not re-derived -- since what IgnoreSimilar
	// keys on here is the two sharing one owner.
	const comp::Allegiance f2Allegiance =
			*b2.reg.try_get<comp::Allegiance>(if1);
	const EntityId if2 = b2.spawn(
			Layer::Field, f2Pos, comp::Motion{}, f2Phys, &mask, f2Allegiance);
	b2.reg.emplace<comp::IgnoreSimilar>(if2);

	b2.step();
	CHECK(!pairCollided(b2, if1, if2),
			"a flame must not hit the ship that fired it, either way round");

	// ...but a different ship's flame, same player or not, does hit.
	Battle b3(1);
	const comp::Position g1Pos = *b2.reg.try_get<comp::Position>(if1);
	const comp::Physique g1Phys = *b2.reg.try_get<comp::Physique>(if1);
	// Transient for the same reason as above: the target is solid, so the
	// flame needs finite life for a stationary overlap to register. The
	// owner is overwritten below, so only playerNr fidelity matters here.
	const EntityId ig1 = b3.spawn(Layer::Field, g1Pos, comp::Motion{}, g1Phys,
			&mask, *b2.reg.try_get<comp::Allegiance>(if1));
	b3.reg.emplace<comp::Lifetime>(ig1, comp::Lifetime{2});
	b3.reg.emplace<comp::IgnoreSimilar>(ig1);

	const comp::Position g2Pos = *b2.reg.try_get<comp::Position>(if2);
	const comp::Physique g2Phys = *b2.reg.try_get<comp::Physique>(if2);
	const EntityId ig2 = b3.spawn(Layer::Field, g2Pos, comp::Motion{}, g2Phys,
			&mask, *b2.reg.try_get<comp::Allegiance>(if2));
	b3.reg.emplace<comp::IgnoreSimilar>(ig2);

	// Two distinct real owners: each element owning itself is enough, since
	// what IgnoreSimilar keys on is only that the owners differ.
	b3.reg.try_get<comp::Allegiance>(ig1)->owner = ig1;
	b3.reg.try_get<comp::Allegiance>(ig2)->owner = ig2;

	b3.step();
	CHECK(pairCollided(b3, ig1, ig2),
			"different owners collide even with IgnoreSimilar on both");
}

// --------------------------------------------------------------------------
// Collision response

void testIsqrtIsFloorSqrt()
{
	CHECK(isqrt(0) == 0, "isqrt(0)");
	CHECK(isqrt(1) == 1, "isqrt(1)");
	CHECK(isqrt(3) == 1, "isqrt floors");
	CHECK(isqrt(4) == 2, "isqrt(4)");
	CHECK(isqrt(0xFFFFFFFFu) == 65535, "isqrt at the top of the range");
	for (u32 r = 1; r < 1000; ++r)
	{
		CHECK(isqrt(r * r) == r, "isqrt of a perfect square %u", r);
		CHECK(isqrt(r * r - 1) == r - 1, "and just below one");
	}
}

void testHeadOnCollisionExchangesMomentum()
{
	// The turn/thrust stagger this test pins is ShipState's own field, and
	// a non-null ShipState* is what tells pure physics "this side is a ship".
	comp::ShipState a;
	const comp::Physique aPhys{5};
	comp::Motion aMotion;
	aMotion.velocity.setComponents(worldToVelocity(20), 0);
	comp::Position aPos;
	aPos.current = Vec2i{0, 0};
	aPos.next = Vec2i{100, 0};

	comp::ShipState b;
	const comp::Physique bPhys{5};
	comp::Motion bMotion;
	bMotion.velocity.setComponents(-worldToVelocity(20), 0);
	comp::Position bPos;
	bPos.current = Vec2i{200, 0};
	bPos.next = Vec2i{110, 0};

	const Vec2i beforeA = aMotion.velocity.current();
	comp::CollisionScratch aScratch, bScratch;
	applyImpulse(aPos, aMotion, aPhys, &a, aScratch, bPos, bMotion, bPhys, &b,
			bScratch);
	const Vec2i afterA = aMotion.velocity.current();

	CHECK(afterA.x < beforeA.x,
			"a head-on hit should reverse the mover, %ld -> %ld",
			static_cast<long>(beforeA.x), static_cast<long>(afterA.x));

	// Ships are staggered by a collision -- that is the recoil you feel.
	CHECK(a.turnWait == kCollisionTurnWait
					&& a.thrustWait == kCollisionThrustWait,
			"a collision should stagger the ship's controls");
}

void testGravityMassIsNotPushed()
{
	// A planet pushes; it is not pushed (element.h:198).
	CHECK(isGravityMass(101), "mass above 100 is a gravity mass");
	CHECK(!isGravityMass(10), "a ship is not");

	comp::ShipState ship;
	const comp::Physique shipPhys{5};
	comp::Motion shipMotion;
	shipMotion.velocity.setComponents(worldToVelocity(20), 0);
	comp::Position shipPos;
	shipPos.current = Vec2i{0, 0};
	shipPos.next = Vec2i{100, 0};

	// The planet is not a ship: no ShipState, so a null pointer -- pure
	// physics needs nothing else to tell the two apart.
	const comp::Physique planetPhys{200};
	comp::Motion planetMotion;
	comp::Position planetPos;
	planetPos.current = Vec2i{200, 0};
	planetPos.next = Vec2i{200, 0};

	comp::CollisionScratch shipScratch, planetScratch;
	applyImpulse(shipPos, shipMotion, shipPhys, &ship, shipScratch, planetPos,
			planetMotion, planetPhys, nullptr, planetScratch);
	CHECK(planetMotion.velocity.isZero(), "the planet must not move");
	CHECK(!shipMotion.velocity.isZero(), "the ship must");
}

void testStuckPairIsWorkedApart()
{
	// Two elements that did not move cannot exchange momentum, so the C marks
	// them DefyPhysics instead. Already defying, it zeroes them and skews the
	// impact axis by an octant -- how two stuck objects come unwelded.
	comp::ShipState a;
	const comp::Physique aPhys{5};
	comp::Motion aMotion;
	comp::Position aPos;
	aPos.current = aPos.next = Vec2i{100, 100};

	comp::ShipState b = a;
	const comp::Physique &bPhys = aPhys;
	comp::Motion bMotion;
	comp::Position bPos;
	bPos.current = bPos.next = Vec2i{100, 100};

	comp::CollisionScratch aScratch, bScratch;
	applyImpulse(aPos, aMotion, aPhys, &a, aScratch, bPos, bMotion, bPhys, &b,
			bScratch);
	CHECK(aScratch.defyPhysics,
			"a stationary pair should defy physics rather than exchange "
			"nothing");
	CHECK(bScratch.defyPhysics, "both of them");

	// Second time round, already defying: velocities are zeroed and the pair
	// gets pushed apart along a skewed axis.
	applyImpulse(aPos, aMotion, aPhys, &a, aScratch, bPos, bMotion, bPhys, &b,
			bScratch);
	CHECK(!aMotion.velocity.isZero() || !bMotion.velocity.isZero(),
			"an already-stuck pair should be given a way out");
}

void testDeriveSpeedStateFromVelocity()
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

void testShipInitialisesFromItsDescriptor()
{
	Battle b(1);
	const EntityId id = spawnPlayerShip(b, earthlingCruiser(), nullptr,
			Vec2i{1000, 1000}, Facing(0), 0, /*warpIn=*/false);

	b.step();
	auto *s = b.ship(id);
	CHECK(s->crew == 18, "the cruiser starts with 18 crew, got %ld",
			static_cast<long>(s->crew));
	CHECK(s->energy == 18, "and 18 energy, got %ld",
			static_cast<long>(s->energy));
}

void testTurningIsGatedByTurnWait()
{
	Battle b(1);
	// The Avenger turns every 2 frames; the Cruiser every 1.
	const EntityId slow = spawnPlayerShip(b, ilwrathAvenger(), nullptr,
			Vec2i{1000, 1000}, Facing(0), 0, /*warpIn=*/false);

	b.step();  // Appearing frame: input is not latched
	b.reg.try_get<comp::Input>(slow)->buttons = ShipInput::Right;

	// turnWait N means a turn every N+1 frames, not every N: the counter is
	// set to N *after* a turn and has to reach zero again (ship.c:238-253).
	// The Avenger's 2 therefore turns on frames 1, 4, 7...
	const Facing start = b.reg.try_get<comp::Position>(slow)->facing;
	b.step();
	CHECK(b.reg.try_get<comp::Position>(slow)->facing == start + 1,
			"the first turn should happen immediately");
	b.step();
	b.step();
	CHECK(b.reg.try_get<comp::Position>(slow)->facing == start + 1,
			"and the next two frames should be spent waiting");
	b.step();
	CHECK(b.reg.try_get<comp::Position>(slow)->facing == start + 2,
			"then turn again on the fourth");
}

void testFiringSpendsEnergyAndRespectsCooldown()
{
	Battle b(1);
	const EntityId id = spawnPlayerShip(b, earthlingCruiser(), nullptr,
			Vec2i{2000, 2000}, Facing(0), 0, /*warpIn=*/false);

	b.step();
	CHECK(b.size() == 1, "just the ship so far");

	b.reg.try_get<comp::Input>(id)->buttons = ShipInput::Weapon;
	b.step();
	CHECK(b.size() == 2, "firing should have spawned a missile");
	CHECK(b.ship(id)->energy == 18 - 9, "and spent 9 energy, got %ld",
			static_cast<long>(b.ship(id)->energy));

	// weapon.wait is 10, so holding the trigger must not fire again yet.
	b.step();
	CHECK(b.size() == 2,
			"the cooldown should block a second shot, got %d elements and "
			"weaponCounter %ld",
			static_cast<int>(b.size()),
			static_cast<long>(b.ship(id)->weaponCounter));

	// With the energy drained below the cost, it must not fire at all -- and
	// must not start a cooldown either.
	Battle c(1);
	const EntityId poor = spawnPlayerShip(c, earthlingCruiser(), nullptr,
			Vec2i{2000, 2000}, Facing(0), 0, /*warpIn=*/false);
	c.step();
	c.ship(poor)->energy = 8;         // one short of the 9-point cost
	c.ship(poor)->energyCounter = 5;  // ...and hold off regen, which
									  // would otherwise top it up first
	c.reg.try_get<comp::Input>(poor)->buttons = ShipInput::Weapon;
	c.step();
	CHECK(c.size() == 1, "a ship that cannot afford the shot must not fire");
	CHECK(c.ship(poor)->energy == 8, "and must not be charged for it, got %ld",
			static_cast<long>(c.ship(poor)->energy));
}

void testMissileFliesAndExpires()
{
	Battle b(1);
	const EntityId id = spawnPlayerShip(b, earthlingCruiser(), nullptr,
			Vec2i{4000, 4000}, Facing(0), 0, /*warpIn=*/false);
	b.step();

	b.reg.try_get<comp::Input>(id)->buttons = ShipInput::Weapon;
	b.step();
	b.reg.try_get<comp::Input>(id)->buttons = ShipInput::None;
	CHECK(b.size() == 2, "one missile");

	// Find it and watch it move.
	EntityId shot = kNoEntity;
	b.eachOrdered([&](EntityId e) {
		if (b.reg.all_of<comp::Warhead>(e))
			shot = e;
	});
	CHECK(shot != kNoEntity, "the missile should be in the list");

	const Vec2i first = b.reg.try_get<comp::Position>(shot)->current;
	b.step();
	const Vec2i second = b.reg.try_get<comp::Position>(shot)->current;
	CHECK(!(first == second), "the missile should move");
	CHECK(second.y < first.y, "and facing 0 is up, so it goes -y");

	// MISSILE_LIFE is 60, so it is gone well before 100 frames.
	for (int i = 0; i < 100 && b.size() > 1; ++i)
		b.step();
	CHECK(b.size() == 1, "the missile should expire");
}

void testFiringPostponesEnergyRegen()
{
	// Every successful energy spend re-arms the regen countdown
	// (status.c:317-323), so firing continuously blocks regeneration. The
	// Avenger (cost 1, wait 0, regen 4/4) would nearly self-sustain without it.
	Battle b(1);
	const EntityId id = spawnPlayerShip(b, ilwrathAvenger(), nullptr,
			Vec2i{2000, 2000}, Facing(0), 0, /*warpIn=*/false);
	b.step();  // Appearing frame

	b.reg.try_get<comp::Input>(id)->buttons = ShipInput::Weapon;
	for (int i = 0; i < 6; ++i)
		b.step();

	// Six shots, no regeneration mixed in: 16 - 6, exactly.
	CHECK(b.ship(id)->energy == 16 - 6,
			"six frames of flame should cost six energy with no regen, got %ld",
			static_cast<long>(b.ship(id)->energy));
}

void testSpecialFiresTheFrameItsCounterExpires()
{
	// The C decrements special_counter and then lets the ship code see the
	// result (ship.c:342-346), so a held special re-fires every special.wait
	// frames -- the frame the counter reaches zero, not the one after.
	//
	// Counted through point defence, since the gate no longer takes an
	// arbitrary hook: its beam is one Laser SpawnEvent per volley, and the
	// mechanic re-arms the counter, which is what makes the period visible.
	static const CollisionMask targetMask = solid(4, 4);
	static ShipSpec const d = [] {
		ShipSpec d_ = earthlingCruiser();
		d_.special.wait = 3;
		d_.special.energyCost = 0;
		return d_;
	}();

	Battle b(1);
	const EntityId id = spawnPlayerShip(
			b, d, nullptr, Vec2i{2000, 2000}, Facing(0), 0, /*warpIn=*/false);

	// A durable target inside LASER_RANGE: a volley burns one hit point, so
	// this one outlives the test and every frame gets a fair chance to fire.
	comp::Position targetPos;
	targetPos.current = targetPos.next = Vec2i{2200, 2000};
	const EntityId target = b.spawn(Layer::Ordnance, targetPos, comp::Motion{},
			comp::Physique{1}, &targetMask);
	b.reg.emplace<comp::Vitality>(target, comp::Vitality{50});

	b.step();  // Appearing frame; ShipMachines forces Input::None on it

	int fires = 0;
	b.reg.get<comp::Input>(id).buttons = ShipInput::Special;
	for (int i = 0; i < 4; ++i)
	{
		b.step();
		for (const SpawnEvent &sp : b.spawns())
			if (sp.kind == SpawnFlavor::Laser)
				++fires;
	}

	// Frame 1 fires with the counter already at zero and re-arms it to 3;
	// frames 2 and 3 spend it; frame 4 lands on zero and fires again.
	CHECK(fires == 2, "a wait of 3 should fire twice in four frames, got %d",
			fires);
}

void testOpposingMissilesDestroyEachOther()
{
	// A weapon's mass is its damage (weapon.c:101); CollisionPossible skips a
	// pair only when BOTH masses are zero (collide.h:38) -- so shots from
	// different ships collide, the mechanism behind flame-intercepts-nuke.
	static CollisionMask shotMask = solid(3, 3);
	static ShipSpec const d = [] {
		ShipSpec d_ = earthlingCruiser();
		d_.weapon.masks = std::span<const CollisionMask>(&shotMask, 1);
		return d_;
	}();

	Battle b(1);
	// Far enough apart that the missiles meet in the middle long before
	// either could reach the opposing ship.
	const EntityId a = spawnPlayerShip(
			b, d, nullptr, Vec2i{4000, 6000}, Facing(0), 0, /*warpIn=*/false);
	const EntityId c = spawnPlayerShip(
			b, d, nullptr, Vec2i{4000, 2000}, Facing(8), 1, /*warpIn=*/false);
	b.step();  // Appearing frame

	b.reg.try_get<comp::Input>(a)->buttons = ShipInput::Weapon;
	b.reg.try_get<comp::Input>(c)->buttons = ShipInput::Weapon;
	b.step();
	b.reg.try_get<comp::Input>(a)->buttons = ShipInput::None;
	b.reg.try_get<comp::Input>(c)->buttons = ShipInput::None;

	usize weapons = 0;
	b.eachOrdered([&](EntityId e) {
		if (b.reg.all_of<comp::Warhead>(e))
			++weapons;
	});
	CHECK(weapons == 2, "both ships should have fired, got %zu", weapons);

	// The nukes close at 80+ world units a frame across a ~3300-unit gap:
	// they must meet and annihilate well before frame 40, and long before
	// either 60-frame life expires or either ship is reached.
	for (int i = 0; i < 40; ++i)
		b.step();

	weapons = 0;
	b.eachOrdered([&](EntityId e) {
		if (b.reg.all_of<comp::Warhead>(e))
			++weapons;
	});
	CHECK(weapons == 0,
			"the missiles should have destroyed each other mid-flight, "
			"%zu still alive",
			weapons);
	CHECK(b.ship(a)->crew == 18 && b.ship(c)->crew == 18,
			"and neither ship should have been hit, got %ld and %ld",
			static_cast<long>(b.ship(a)->crew),
			static_cast<long>(b.ship(c)->crew));
}

void testTheTwoShipsFeelDifferent()
{
	// Not a rendering claim -- these are the numbers that make the Avenger
	// play nothing like the Cruiser, and they come straight from the C.
	const ShipSpec &cruiser = earthlingCruiser();
	const ShipSpec &avenger = ilwrathAvenger();

	CHECK(cruiser.thrustWait == 4 && avenger.thrustWait == 0,
			"the Avenger accelerates every frame, the Cruiser every fourth");
	CHECK(cruiser.weapon.wait == 10 && avenger.weapon.wait == 0,
			"the Avenger's flame is continuous, the Cruiser's missiles are "
			"not");
	CHECK(cruiser.turnWait == 1 && avenger.turnWait == 2,
			"but the Cruiser turns twice as fast");
	CHECK(cruiser.weapon.energyCost == 9 && avenger.weapon.energyCost == 1,
			"and pays 9 a shot against the Avenger's 1");
}

// --------------------------------------------------------------------------
// Gravity and the battlefield

EntityId addPlanet(Battle &b, const CollisionMask &m, Vec2i at)
{
	// Allegiance defaults to NEUTRAL_PLAYER_NUM/no owner.
	const comp::Physique phys{200};
	comp::Position pos;
	pos.current = at;
	pos.next = at;
	const EntityId id = b.spawn(Layer::Field, pos, comp::Motion{}, phys, &m);
	b.reg.emplace<comp::Indestructible>(id);
	b.reg.emplace<comp::Vitality>(id, comp::Vitality{200});
	return id;
}

void testGravityMassThreshold()
{
	CHECK(kGravityMass == 100, "MAX_SHIP_MASS * 10");
	CHECK(isGravitySource(200), "a planet (mass 200) is a gravity source");
	CHECK(!isGravitySource(6), "the Cruiser is not");
	CHECK(!isGravitySource(3), "nor is an asteroid");
	CHECK(!isGravitySource(99), "nor is anything below the threshold");

	// The `+ 1` gravity.c applies before every test: a fleeing ship is set to
	// exactly MAX_SHIP_MASS * 10 (battle.c:92), which fails plain GRAVITY_MASS
	// but passes this -- exactly what stops a planet dragging in the escapee.
	CHECK(isGravitySource(kGravityMass),
			"a fleeing ship counts as a source, so nothing pulls on it");
}

void testGravityPullsTowardTheSource()
{
	const CollisionMask m = solid(4, 4);
	Battle b(1);
	const EntityId planet = addPlanet(b, m, Vec2i{4000, 4000});

	// 100 world units to the planet's right, well inside the 1020-unit disc.
	const EntityId ship = spawnPlayerShip(b, earthlingCruiser(), nullptr,
			Vec2i{4100, 4000}, Facing(0), 0, /*warpIn=*/false);
	b.reg.emplace<comp::Collider>(ship, &m);
	b.ship(ship)->speed = SpeedState::AtMax;

	CHECK(!calculateGravity(b, planet), "the source itself is never in a well");

	const Vec2i v = b.reg.try_get<comp::Motion>(ship)->velocity.current();
	CHECK(v.x < 0, "the ship should be pulled back toward the planet, got %ld",
			static_cast<long>(v.x));
	CHECK(v.y == 0, "and straight along the axis, got %ld",
			static_cast<long>(v.y));
	CHECK(b.ship(ship)->inGravityWell,
			"and should be flagged as being in the well");
	CHECK(b.ship(ship)->speed == SpeedState::Normal,
			"gravity.c:136 clears at-max so the ship may accelerate again");

	// And the other direction: asked of the light element, it reports the well.
	CHECK(calculateGravity(b, ship), "the ship should know it is in a well");
}

void testGravityHasAHardEdge()
{
	const CollisionMask m = solid(4, 4);

	// GRAVITY_THRESHOLD is 255 *display* pixels (ONE_SHIFT 2): a 1020-unit
	// disc with no falloff inside and nothing outside -- the DIFUSE_GRAVITY
	// smoothing block is commented out in the C (gravity.c:96-111).
	for (const auto &[dx, pulled] :
			{std::pair{1020, true}, std::pair{1024, false}})
	{
		Battle b(1);
		const EntityId planet = addPlanet(b, m, Vec2i{4000, 4000});
		const EntityId ship = spawnPlayerShip(b, earthlingCruiser(), nullptr,
				Vec2i{4000 + dx, 4000}, Facing(0), 0, /*warpIn=*/false);
		b.reg.emplace<comp::Collider>(ship, &m);

		(void)calculateGravity(b, planet);
		const bool moved =
				!b.reg.try_get<comp::Motion>(ship)->velocity.isZero();
		CHECK(moved == pulled, "at %d world units the pull should be %s", dx,
				pulled ? "on" : "off");
	}
}

void testFleeingShipIsImmuneToGravity()
{
	const CollisionMask m = solid(4, 4);
	Battle b(1);
	const EntityId planet = addPlanet(b, m, Vec2i{4000, 4000});
	const EntityId ship = spawnPlayerShip(b, earthlingCruiser(), nullptr,
			Vec2i{4100, 4000}, Facing(0), 0, /*warpIn=*/false);
	b.reg.emplace<comp::Collider>(ship, &m);
	b.reg.try_get<comp::Physique>(ship)->mass =
			kGravityMass;  // DoRunAway, battle.c:92

	(void)calculateGravity(b, planet);
	CHECK(b.reg.try_get<comp::Motion>(ship)->velocity.isZero(),
			"a ship at mass 100 reads as a source, so the planet skips it");
}

void testAsteroidsSpawnOnAnEdgeAndRepeatably()
{
	const CollisionMask m = solid(4, 4);

	Battle b(7);
	Battle c(7);
	for (int i = 0; i < kNumAsteroids; ++i)
	{
		const EntityId a = spawnAsteroid(b, &m);
		const EntityId a2 = spawnAsteroid(c, &m);

		const comp::Position *pos = b.reg.try_get<comp::Position>(a);
		const bool onEdge = pos->current.x == 0 || pos->current.x == kArena.w
				|| pos->current.y == 0 || pos->current.y == kArena.h;
		CHECK(onEdge, "asteroid %d should enter from an edge, got (%ld,%ld)", i,
				static_cast<long>(pos->current.x),
				static_cast<long>(pos->current.y));
		CHECK(!b.reg.try_get<comp::Motion>(a)->velocity.isZero(),
				"and should be moving");

		// Same seed, same asteroid: the seven draws happen in a fixed order.
		// The spin lives on its own component, so the pin compares it there --
		// not a vacuous 0 == 0.
		const comp::Position *pos2 = c.reg.try_get<comp::Position>(a2);
		const comp::Spin &s1 = *b.reg.try_get<comp::Spin>(a);
		const comp::Spin &s2 = *c.reg.try_get<comp::Spin>(a2);
		CHECK(pos->current == pos2->current && pos->facing == pos2->facing
						&& s1.period == s2.period
						&& s1.backwards == s2.backwards
						&& s1.countdown == s2.countdown,
				"asteroid %d should be identical from the same seed", i);
	}
}

void testAsteroidTumbles()
{
	const CollisionMask m = solid(4, 4);
	Battle b(3);
	const EntityId a = spawnAsteroid(b, &m);

	// The spin period means "every N+1 frames", like turn_wait
	// (misc.c:117-126); it lives in the Spin component.
	const int period = static_cast<int>(b.reg.try_get<comp::Spin>(a)->period);
	const Facing start = b.reg.try_get<comp::Position>(a)->facing;

	// period + 1 frames of stillness, not period: the first step is the
	// asteroid's appearing frame and an asteroid is not a PLAYER_SHIP, so its
	// preprocess hook does not run at all that frame (process.c:150-154).
	for (int i = 0; i < period + 1; ++i)
		b.step();
	CHECK(b.reg.try_get<comp::Position>(a)->facing == start,
			"it should hold still for %d frames", period + 1);

	b.step();
	CHECK(b.reg.try_get<comp::Position>(a)->facing != start,
			"then rotate one frame index");
}

void testDestroyedAsteroidIsReplaced()
{
	// The field's population is constant because the loop is closed: an
	// asteroid's death leaves rubble, and the rubble's death spawns a fresh
	// asteroid (misc.c:80-105, 130-201) -- break either link and it empties
	// out.
	const CollisionMask m = solid(4, 4);
	Battle b(11);
	(void)spawnAsteroid(b, &m);
	CHECK(b.size() == 1, "one asteroid to start");

	// do_damage's kill (misc.c:220-222).
	b.eachOrdered([&](EntityId e) {
		b.reg.try_get<comp::Vitality>(e)->hitPoints = 0;
		b.reg.emplace<comp::Lifetime>(e, comp::Lifetime{0});
		b.reg.remove<comp::Collider>(e);
	});

	int asteroids = 0;
	for (int i = 0; i < 12; ++i)
	{
		b.step();
		asteroids = 0;
		b.eachOrdered([&](EntityId e) {
			if (b.reg.all_of<comp::Spin>(e))
				++asteroids;
		});
		if (asteroids == 1)
			break;
	}
	CHECK(asteroids == 1, "the field should refill itself, got %d asteroids",
			asteroids);
}

void testPlanetPlacementAvoidsEverything()
{
	const CollisionMask m = solid(4, 4);
	Battle b(5);

	// A ship already on the field. The planet must not land in its lap, and
	// must not land close enough that the ship starts the match in a well.
	const EntityId ship = spawnPlayerShip(b, earthlingCruiser(), nullptr,
			Vec2i{4000, 4000}, Facing(0), 0, /*warpIn=*/false);
	b.reg.emplace<comp::Collider>(ship, &m);

	const EntityId planet = spawnPlanet(b, &m);
	CHECK(b.reg.try_get<comp::Physique>(planet)->mass == 200,
			"mass is assigned after placement, got %ld",
			static_cast<long>(b.reg.try_get<comp::Physique>(planet)->mass));
	CHECK(!timeSpaceMatterConflict(b, planet),
			"the planet should not overlap the ship");
	CHECK(!calculateGravity(b, ship),
			"and the ship should not start inside its well");
}

// --------------------------------------------------------------------------
// Damage

void testDeltaCrewReportsDeathOnTheExactHit()
{
	Battle b(1);
	const EntityId id = spawnPlayerShip(b, earthlingCruiser(), nullptr,
			Vec2i{1000, 1000}, Facing(0), 0, /*warpIn=*/false);
	b.step();  // the appearing frame is what loads crew from the descriptor

	CHECK(b.ship(id)->crew == 18, "the Cruiser starts with 18 crew, got %ld",
			static_cast<long>(b.ship(id)->crew));

	CHECK(deltaCrew(*b.ship(id), -4), "losing 4 of 18 is survivable");
	CHECK(b.ship(id)->crew == 14, "and leaves 14, got %ld",
			static_cast<long>(b.ship(id)->crew));

	// status.c:357 compares with a strict `>`, so losing exactly what is left
	// is death, not a ship sitting at zero crew. Off by one here and a ship
	// survives its own destruction.
	CHECK(!deltaCrew(*b.ship(id), -14),
			"losing exactly the remainder is death");
	CHECK(b.ship(id)->crew == 0, "and leaves nothing");

	// Repair cannot exceed the maximum.
	CHECK(deltaCrew(*b.ship(id), 100), "repair always succeeds");
	CHECK(b.ship(id)->crew == 18, "and is capped at max, got %ld",
			static_cast<long>(b.ship(id)->crew));
}

void testPlanetsTakeNoDamage()
{
	// doDamage needs a Battle to fetch crew through, so each subject here is
	// spawned rather than built standalone.
	Battle b(1);

	const EntityId planetId = b.spawn(Layer::Field, comp::Position{},
			comp::Motion{}, comp::Physique{200});
	b.reg.emplace<comp::Indestructible>(planetId);
	b.reg.emplace<comp::Vitality>(planetId, comp::Vitality{200});

	doDamage(b, planetId, 50);
	auto *pv = b.reg.try_get<comp::Vitality>(planetId);
	CHECK(pv->hitPoints == 200, "a planet is not damageable, got %ld",
			static_cast<long>(pv->hitPoints));
	CHECK(!isTransient(b, planetId), "and holds no Lifetime to kill");

	// weaponCollision has its own target-survives check (Damage.cpp), separate
	// from doDamage's mass guard above -- a regression in one would not be
	// caught by testing only the other.
	const EntityId shotId = b.spawn(Layer::Field, comp::Position{},
			comp::Motion{}, comp::Physique{4});  // damage == mass, weapon.c:144
	b.reg.emplace<comp::Vitality>(shotId, comp::Vitality{});
	b.reg.emplace<comp::Warhead>(shotId, comp::Warhead{});
	weaponCollision(b, shotId, planetId);

	CHECK(b.reg.try_get<comp::Vitality>(planetId)->hitPoints == 200,
			"weaponCollision must not dent the planet either, got %ld",
			static_cast<long>(
					b.reg.try_get<comp::Vitality>(planetId)->hitPoints));
	CHECK(b.reg.all_of<comp::Doomed>(shotId),
			"the shot still spends itself on impact");

	// The same call, asked of a ship that has fled to mass 100. isGravityMass
	// is the predicate *without* gravity.c's `+ 1`, so it stays damageable
	// even while gravity treats it as a source.
	const EntityId fleeingId = b.spawn(Layer::Field, comp::Position{},
			comp::Motion{}, comp::Physique{kGravityMass});
	b.reg.emplace<comp::Vitality>(fleeingId, comp::Vitality{10});

	doDamage(b, fleeingId, 4);
	auto *fv = b.reg.try_get<comp::Vitality>(fleeingId);
	CHECK(fv->hitPoints == 6, "a fleeing ship is still damageable, got %ld",
			static_cast<long>(fv->hitPoints));
}

void testMissileDamagesAndSpendsItself()
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
	const EntityId gunner = spawnPlayerShip(b, cruiser, nullptr,
			Vec2i{4000, 4000}, Facing(0), 0, /*warpIn=*/false);
	b.reg.emplace<comp::Collider>(gunner, &m);

	// 400 world units away, not 100: HUMAN_OFFSET is 42 *display* pixels
	// (168 world units), so the missile is born that far out already -- a
	// closer target would spawn past, and the missile would sail off untouched.
	const EntityId target = spawnPlayerShip(b, ilwrathAvenger(), nullptr,
			Vec2i{4000, 3600}, Facing(8), 1, /*warpIn=*/false);
	b.reg.emplace<comp::Collider>(target, &m);
	b.step();

	const i32 before = b.ship(target)->crew;
	CHECK(before == 22, "the Avenger starts with 22 crew, got %ld",
			static_cast<long>(before));

	b.reg.try_get<comp::Input>(gunner)->buttons = ShipInput::Weapon;
	b.step();
	b.reg.try_get<comp::Input>(gunner)->buttons = ShipInput::None;

	// The missile flies -40 a frame from y=3832, so it reaches y=3600 in about
	// six. MISSILE_DAMAGE is 4.
	for (int i = 0; i < 12 && b.ship(target)->crew == before; ++i)
		b.step();

	CHECK(b.ship(target)->crew == before - 4,
			"the missile should cost 4 crew, got %ld",
			static_cast<long>(b.ship(target)->crew));

	// One more frame: the walk preprocessed the missile before its hit, so
	// its zeroed life is only seen -- and the wreck reaped -- on the next
	// death check, exactly as in the C.
	b.step();

	int weapons = 0;
	int blasts = 0;
	b.eachOrdered([&](EntityId e) {
		if (b.reg.all_of<comp::Warhead>(e))
			++weapons;
		if (b.reg.all_of<comp::Blast>(e))
			++blasts;
	});
	CHECK(weapons == 0, "the missile should be spent, got %d", weapons);
	CHECK(blasts == 1, "and should have left a blast, got %d", blasts);
}

void testFlyingIntoAPlanetCostsCrewOverFour()
{
	const CollisionMask m = solid(8, 8);
	Battle b(1);

	comp::Position planetPos;
	planetPos.current = Vec2i{4000, 4000};
	planetPos.next = planetPos.current;
	const EntityId planetId = b.spawn(
			Layer::Field, planetPos, comp::Motion{}, comp::Physique{200}, &m);
	b.reg.emplace<comp::Indestructible>(planetId);
	b.reg.emplace<comp::Vitality>(planetId, comp::Vitality{200});

	// Spawned clear of the planet, then moved into it -- not a shortcut:
	// starting overlapped resolves the collision before the ship's first
	// preprocess loads crew, killing it at zero (misc.c:63-70, ship.c:480).
	const EntityId ship = spawnPlayerShip(b, earthlingCruiser(), nullptr,
			Vec2i{5000, 5000}, Facing(0), 0, /*warpIn=*/false);
	b.reg.emplace<comp::Collider>(ship, &m);
	b.step();

	CHECK(b.alive(ship), "the ship should have survived setup");
	if (!b.alive(ship))
		return;

	const i32 before = b.ship(ship)->crew;

	// Fly into it rather than teleport into overlap: an at-rest overlap is
	// the "BAD NEWS" case the step deliberately skips (process.c:397-416),
	// so contact has to happen mid-motion, the way it does in play.
	b.reg.try_get<comp::Position>(ship)->current = Vec2i{4000, 4064};
	b.reg.try_get<comp::Position>(ship)->next =
			b.reg.try_get<comp::Position>(ship)->current;
	b.reg.try_get<comp::Motion>(ship)->velocity.setComponents(
			0, -worldToVelocity(40));
	b.step();
	CHECK(b.alive(ship), "and should survive one planet graze");
	if (!b.alive(ship))
		return;

	// ship.c:364-367 computes hit_points >> 2, floored at 1 -- and for a
	// PLAYER_SHIP, hit_points IS crew_level (element.h:126-133), one union
	// field. An 18-crew Cruiser pays 4 crew for a planet graze, not 1.
	CHECK(b.ship(ship)->crew == before - (before >> 2),
			"hitting a planet should cost crew/4 (%ld), got %ld (was %ld)",
			static_cast<long>(before >> 2),
			static_cast<long>(b.ship(ship)->crew), static_cast<long>(before));
}

void testOverlappingShipsSeparateInsteadOfSticking()
{
	// Two ships interpenetrating -- the tail of a collision just resolved --
	// are not a new collision; the C skips such a pair outright ("BAD NEWS",
	// process.c:397-416, 509-515), or the frozen pair re-welds every frame.
	static const CollisionMask m = solid(8, 8);
	Battle b(1);

	comp::Position aPos;
	aPos.current = aPos.next = Vec2i{4000, 4000};
	const EntityId ia = b.spawn(Layer::Field, aPos, comp::Motion{},
			comp::Physique{6}, &m, comp::Allegiance{0, kNoEntity});

	comp::Position cPos;
	cPos.current = cPos.next = Vec2i{6000, 6000};
	const EntityId ic = b.spawn(Layer::Field, cPos, comp::Motion{},
			comp::Physique{6}, &m, comp::Allegiance{1, kNoEntity});

	// Spawned far apart and established first: an APPEARING element found
	// overlapping something is EXECUTED on the spot in the C (process.c:
	// 427-449), so the skip under test applies only past the spawn frame.
	b.step();

	// Now manufacture the post-impact state: overlapping -- 16 world units
	// is 4 display pixels, half a mask -- and moving apart, exactly what an
	// impact response leaves behind.
	b.reg.try_get<comp::Position>(ia)->current =
			b.reg.try_get<comp::Position>(ia)->next = Vec2i{4000, 4000};
	b.reg.try_get<comp::Motion>(ia)->velocity.setComponents(
			-worldToVelocity(20), 0);
	b.reg.try_get<comp::Position>(ic)->current =
			b.reg.try_get<comp::Position>(ic)->next = Vec2i{4016, 4000};
	b.reg.try_get<comp::Motion>(ic)->velocity.setComponents(
			worldToVelocity(20), 0);

	b.step();
	CHECK(b.collisions().empty(), "an at-rest overlap is not a collision");
	CHECK(b.reg.try_get<comp::Position>(ia)->current.x == 3980,
			"the left ship keeps its full motion, got %ld",
			static_cast<long>(b.reg.try_get<comp::Position>(ia)->current.x));
	CHECK(b.reg.try_get<comp::Position>(ic)->current.x == 4036,
			"and the right ship its own, got %ld",
			static_cast<long>(b.reg.try_get<comp::Position>(ic)->current.x));

	// And they stay separated: no re-collision as they clear each other.
	for (int i = 0; i < 10; ++i)
	{
		b.step();
		CHECK(b.collisions().empty(),
				"separating ships must not collide again (frame %d)", i);
	}
	CHECK(b.reg.try_get<comp::Position>(ic)->current.x
							- b.reg.try_get<comp::Position>(ia)->current.x
					> 32 * kScaledOne,
			"ten frames later they are well clear of each other");
}

// A live weapon shot -- FiniteLife, mass-as-damage, mask, Warhead -- the
// shape testShipShotMidFlightKeepsItsMotion and testToughWeaponPiercesWeakOne
// both hand-built. vx/vy are already velocity-space (worldToVelocity'd).
EntityId spawnTestShot(Battle &b, const CollisionMask &mask, Vec2i at,
		i32 playerNr, i32 mass, i32 hitPoints, i32 lifeSpan, i32 vx = 0,
		i32 vy = 0)
{
	comp::Position pos;
	pos.current = pos.next = at;
	comp::Motion motion;
	motion.velocity.setComponents(vx, vy);
	const EntityId id = b.spawn(Layer::Field, pos, motion, comp::Physique{mass},
			&mask, comp::Allegiance{playerNr, kNoEntity});
	b.reg.emplace<comp::Lifetime>(id, comp::Lifetime{lifeSpan});
	b.reg.emplace<comp::Vitality>(id, comp::Vitality{hitPoints});
	// weaponCollision reads Warhead unconditionally when it detonates, same
	// as production's fire block attaches to every shot; this helper never
	// exercised damage/blastOffset's values, so both default to zero.
	b.reg.emplace<comp::Warhead>(id, comp::Warhead{});
	return id;
}

void testShipShotMidFlightKeepsItsMotion()
{
	// A ship hit by a weapon is not stopped by it: only an element whose own
	// collision_func raised COLLISION moves to the impact point (process.c:
	// 586-596), and a ship's does so only when solid (ship.c:356-358).
	static const CollisionMask m = solid(8, 8);
	Battle b(1);

	comp::Position shipPos;
	shipPos.current = shipPos.next = Vec2i{4000, 4000};
	const EntityId is = b.spawn(Layer::Field, shipPos, comp::Motion{},
			comp::Physique{6}, &m, comp::Allegiance{0, kNoEntity});
	// Empty spec: facingMasks stays empty, so ShipMachines' Appearing
	// branch never reattaches a Collider over `m`, which this test's
	// impact-point math is keyed to.
	static const ShipSpec inertSpec{};
	b.attachShip(is, &inertSpec);

	// A stationary shot in the ship's path, zero damage: the run is about
	// motion, not crew. Damage IS mass (weapon.c:101,144), so this shot is
	// massless, but the pair still collides on the ship's own mass.
	const EntityId iw = spawnTestShot(b, m, Vec2i{4200, 4000}, 1,
			/*mass=*/0, /*hitPoints=*/0, /*lifeSpan=*/20);

	b.step();  // spawn frame: 50 display pixels apart, nothing touches

	b.reg.try_get<comp::Motion>(is)->velocity.setComponents(
			worldToVelocity(80), 0);
	bool hit = false;
	for (int i = 0; i < 6 && !hit; ++i)
	{
		const i32 beforeX = b.reg.try_get<comp::Position>(is)->current.x;
		b.step();
		if (!b.collisions().empty())
		{
			hit = true;
			CHECK(b.reg.try_get<comp::Position>(is)->current.x == beforeX + 80,
					"the ship keeps its full motion through a weapon hit, "
					"got %ld from %ld",
					static_cast<long>(
							b.reg.try_get<comp::Position>(is)->current.x),
					static_cast<long>(beforeX));
		}
	}
	CHECK(hit, "the ship should have crossed the shot");

	const Vec2i v = b.reg.try_get<comp::Motion>(is)->velocity.current();
	CHECK(velocityToWorld(v.x) == 80,
			"and its velocity untouched -- weapons carry no impulse, got %ld",
			static_cast<long>(velocityToWorld(v.x)));
	// The shot is spent AND gone: weapon_collision marks a missile
	// DISAPPEARING (weapon.c:175-177), reaped the same frame (process.c:
	// 873-879). Warhead::lingersOnHit is the flame's one-frame exception.
	CHECK(!b.alive(iw), "a spent missile is reaped on the frame it hit");
}

void testToughWeaponPiercesWeakOne()
{
	// The pierce rule (weapon.c:161-164): a weapon whose hit points exceed
	// a surviving target's mass ploughs through and keeps flying. Nuke and
	// flame (one hit point each) can never do this -- Chmmr zapsats live on it.
	static const CollisionMask m = solid(3, 3);
	Battle b(1);

	// damage IS mass (weapon.c:101,144).
	const EntityId it = spawnTestShot(b, m, Vec2i{3800, 4000}, 0,
			/*mass=*/2, /*hitPoints=*/3, /*lifeSpan=*/30, worldToVelocity(40));
	const EntityId iw = spawnTestShot(b, m, Vec2i{4200, 4000}, 1,
			/*mass=*/1, /*hitPoints=*/1, /*lifeSpan=*/30, -worldToVelocity(40));

	// They close at 80 a frame across 400 units: contact by frame 6.
	for (int i = 0; i < 8; ++i)
		b.step();

	CHECK(!b.alive(iw), "the weak shot should be destroyed");
	CHECK(b.alive(it), "and the tough one should still be flying");
	if (!b.alive(it))
		return;
	CHECK(b.reg.try_get<comp::Vitality>(it)->hitPoints == 2,
			"having paid the weak shot's damage from its hit points, got %ld",
			static_cast<long>(b.reg.try_get<comp::Vitality>(it)->hitPoints));
	// They meet near x=4000 at frame 5; eight frames of unimpeded flight from
	// 3800 is 4120 -- well past the impact point, with no truncation.
	CHECK(b.reg.try_get<comp::Position>(it)->current.x == 3800 + 8 * 40,
			"and carried on past the impact point unimpeded, got %ld",
			static_cast<long>(b.reg.try_get<comp::Position>(it)->current.x));
}

void testTurningIntoOverlapIsReverted()
{
	// The overlap-repair protocol (process.c:453-506): when a silhouette
	// CHANGE creates a standing overlap -- a ship rotating against a wall --
	// the C reverts the frame and facing rather than resolving a collision.
	static std::array<CollisionMask, 2> masks = [] {
		return std::array<CollisionMask, 2>{solid(4, 4), solid(16, 16)};
	}();
	static ShipSpec const d = [] {
		ShipSpec d_ = earthlingCruiser();
		d_.turnWait = 0;
		d_.facingMasks = std::span<const CollisionMask>(masks);
		return d_;
	}();
	static const CollisionMask wall = solid(16, 16);

	Battle b(1);

	comp::Position planetPos;
	planetPos.current = planetPos.next = Vec2i{4000, 4000};
	const EntityId planetId = b.spawn(Layer::Field, planetPos, comp::Motion{},
			comp::Physique{200}, &wall);
	b.reg.emplace<comp::Indestructible>(planetId);
	b.reg.emplace<comp::Vitality>(planetId, comp::Vitality{200});

	// Adjacent at facing 0 (a 4x4 mask), overlapping at facing 1 (16x16).
	const EntityId ship = spawnPlayerShip(
			b, d, nullptr, Vec2i{4052, 4000}, Facing(0), 0, /*warpIn=*/false);
	b.step();
	CHECK(b.collisions().empty(), "setup: adjacent is not touching");

	const i32 crew = b.ship(ship)->crew;
	b.reg.try_get<comp::Input>(ship)->buttons = ShipInput::Right;
	b.step();

	CHECK(b.reg.try_get<comp::Position>(ship)->facing == Facing(0),
			"the turn into the wall should have been undone, facing %d",
			b.reg.try_get<comp::Position>(ship)->facing.raw());
	CHECK(b.collisions().empty(), "and a reverted turn is not a collision");
	CHECK(b.ship(ship)->crew == crew, "so it costs nothing, got %ld (was %ld)",
			static_cast<long>(b.ship(ship)->crew), static_cast<long>(crew));
}

void testSpawnInsideSomethingIsExecuted()
{
	// The other half of the protocol (process.c:427-449): an APPEARING solid
	// found standing inside another solid dies on the spot, death hook and
	// all -- an asteroid respawned onto the planet becomes rubble at once.
	static const CollisionMask m = solid(8, 8);
	Battle b(1);

	comp::Position planetPos;
	planetPos.current = planetPos.next = Vec2i{4000, 4000};
	const EntityId planetId = b.spawn(
			Layer::Field, planetPos, comp::Motion{}, comp::Physique{200}, &m);
	b.reg.emplace<comp::Indestructible>(planetId);
	b.reg.emplace<comp::Vitality>(planetId, comp::Vitality{200});
	b.step();  // established

	comp::Position rockPos;
	rockPos.current = rockPos.next = Vec2i{4004, 4000};  // inside the planet
	// NORMAL_LIFE, persistent: no Lifetime at all
	const EntityId ir = b.spawn(
			Layer::Field, rockPos, comp::Motion{}, comp::Physique{3}, &m);
	b.reg.emplace<comp::Vitality>(ir, comp::Vitality{1});
	b.reg.emplace<comp::Asteroid>(
			ir, comp::Asteroid{&m, comp::Asteroid::Phase::Solid});

	b.step();

	CHECK(!b.alive(ir),
			"a solid spawned inside another is destroyed the same frame");
	const int rubble = rocksInPhase(b, comp::Asteroid::Phase::Rubble);
	CHECK(rubble == 1, "with its death response run, got %d rubble", rubble);
}

void testPointDefenceBurnsOwnNuke()
{
	// The C's point defence has no ownership filter (human.c:203-204): a
	// Cruiser holding SPECIAL pays for and shoots down its OWN in-flight
	// nuke -- a real tactical constraint, not a bug, decided faithful.
	static CollisionMask shotMask = solid(3, 3);
	static ShipSpec const d = [] {
		ShipSpec d_ = earthlingCruiser();
		d_.weapon.masks = std::span<const CollisionMask>(&shotMask, 1);
		return d_;
	}();

	Battle b(1);
	const EntityId ship = spawnPlayerShip(
			b, d, nullptr, Vec2i{4000, 4000}, Facing(0), 0, /*warpIn=*/false);
	b.step();

	b.reg.try_get<comp::Input>(ship)->buttons = ShipInput::Weapon;
	b.step();
	usize weapons = 0;
	b.eachOrdered([&](EntityId e) {
		if (b.reg.all_of<comp::Warhead>(e))
			++weapons;
	});
	CHECK(weapons == 1, "setup: one nuke in flight, got %zu", weapons);

	const i32 energy = b.ship(ship)->energy;
	b.reg.try_get<comp::Input>(ship)->buttons = ShipInput::Special;
	b.step();
	b.reg.try_get<comp::Input>(ship)->buttons = ShipInput::None;
	b.step();  // the burned nuke's death is seen the following frame

	weapons = 0;
	b.eachOrdered([&](EntityId e) {
		if (b.reg.all_of<comp::Warhead>(e))
			++weapons;
	});
	CHECK(weapons == 0, "the Cruiser's own nuke should be shot down, %zu left",
			weapons);
	CHECK(b.ship(ship)->energy == energy - 4,
			"and the laser paid for (special cost 4), got %ld (was %ld)",
			static_cast<long>(b.ship(ship)->energy), static_cast<long>(energy));
}

void testCommittedElementsAreNotIntegratedTwice()
{
	// The C's POST_PROCESS flag protects a committed element from the
	// whole-list catch-up walks (process.c:859): without it, a ship firing
	// every frame gets preprocessed twice -- double motion, turning, energy.
	Battle b(1);
	const EntityId id = spawnPlayerShip(b, ilwrathAvenger(), nullptr,
			Vec2i{4000, 6000}, Facing(0), 0, /*warpIn=*/false);
	b.step();

	// Turn and fire together. The Avenger turns every turnWait+1 = 3 frames:
	// frames 1, 4, 7, 10 -- four steps in twelve. A double-preprocessed ship
	// turns visibly faster.
	const Facing start = b.reg.try_get<comp::Position>(id)->facing;
	b.reg.try_get<comp::Input>(id)->buttons =
			ShipInput::Right | ShipInput::Weapon;
	for (int i = 0; i < 12; ++i)
		b.step();

	CHECK(b.reg.try_get<comp::Position>(id)->facing == start + 4,
			"twelve frames of turn-and-fire should turn exactly 4 facings, "
			"got %d from %d",
			b.reg.try_get<comp::Position>(id)->facing.raw(), start.raw());
}

void testDefyPhysicsExpires()
{
	// DEFY_PHYSICS lasts from a collision to the next frame without one
	// (process.c:824-829). Held forever, it would disable the post-collision
	// stagger and steer later stationary contacts into the stuck-pair branch.
	Battle b(1);
	const EntityId id = b.spawn(Layer::Field);
	b.reg.try_get<comp::CollisionScratch>(id)->defyPhysics = true;
	b.step();
	CHECK(!b.reg.try_get<comp::CollisionScratch>(id)->defyPhysics,
			"a frame without a collision sheds DefyPhysics");
}

void testPointDefenceBurnsIncomingFire()
{
	const CollisionMask m = solid(8, 8);
	Battle b(1);

	const EntityId ship = spawnPlayerShip(b, earthlingCruiser(), nullptr,
			Vec2i{4000, 4000}, Facing(0), 0, /*warpIn=*/false);
	b.reg.emplace<comp::Collider>(ship, &m);
	b.step();

	// An enemy shot well inside LASER_RANGE, which is 100 display pixels --
	// 400 world units.
	comp::Position shotPos;
	shotPos.current = shotPos.next = Vec2i{4000, 4200};
	const EntityId incoming = b.spawn(Layer::Field, shotPos, comp::Motion{},
			comp::Physique{1}, &m, comp::Allegiance{1, kNoEntity});
	b.reg.emplace<comp::Lifetime>(incoming, comp::Lifetime{20});
	b.reg.emplace<comp::Vitality>(incoming, comp::Vitality{1});
	b.reg.emplace<comp::Warhead>(incoming, comp::Warhead{1, 0});

	b.reg.try_get<comp::Input>(ship)->buttons = ShipInput::Special;
	b.step();

	CHECK(!b.alive(incoming)
					|| b.reg.try_get<comp::Vitality>(incoming)->hitPoints == 0,
			"point defence should have burned the incoming shot");
	CHECK(b.ship(ship)->specialCounter > 0, "and started its cooldown");

	// The beam is a real element, so the renderer has nothing to invent and
	// a replay draws exactly what the original did.
	int beams = 0;
	b.eachOrdered([&](EntityId e) {
		if (b.reg.all_of<comp::Beam>(e))
			++beams;
	});
	CHECK(beams == 1, "and left a beam to draw, got %d", beams);
}

void testDeadShipBurnsAsAPhaseThenGoes()
{
	Battle b(1);
	const EntityId id = spawnPlayerShip(b, earthlingCruiser(), nullptr,
			Vec2i{4000, 4000}, Facing(0), 0, /*warpIn=*/false);
	b.step();

	// doDamage on a crewed hull only accumulates DamageIncoming now; it
	// takes the sync point of the next step() to sum it, check death, and
	// start the explosion.
	doDamage(b, id, 100);
	b.step();
	CHECK(b.reg.all_of<comp::Exploding>(id),
			"overkill starts the explosion phase");

	b.step();
	CHECK(b.alive(id), "the wreck persists while it burns");
	CHECK(b.size() > 1, "and throws debris sparks");

	// Exploding::kLife of burning, then the reap; sparks outlive by
	// Debris::kLife.
	for (int i = 0; i < comp::Exploding::kLife + comp::Debris::kLife + 2; ++i)
		b.step();
	CHECK(!b.alive(id), "then the wreck is reaped");
	CHECK(b.size() == 0, "and the sparks have burned out");
}

void testShipWarpsInBeforeItIsSolid()
{
	// Checked without a window: the effect shipped once invisible against a
	// stationary hull, and screenshot verification would race a 15-frame
	// window against process start. Stepping the battle asks it directly.
	sim::Battle b{7u};

	// A hull to be shaped like. Headless, so there is no sprite to take a
	// real silhouette from, but the shadow only has to *carry* it.
	static const sim::CollisionMask hull = solid(12, 12);

	const sim::EntityId shipId =
			sim::spawnPlayerShip(b, sim::earthlingCruiser(), &hull,
					Vec2i{4000, 4000}, sim::Facing(4), 0, /*warpIn=*/true);

	const auto ship = [&b]() {
		bool found = false;
		b.eachOrdered([&](sim::EntityId id) {
			if (!found && b.reg.all_of<sim::comp::ShipState>(id))
				found = true;
		});
		return found;
	};
	const auto shadows = [&b]() {
		int n = 0;
		b.eachOrdered([&](sim::EntityId id) {
			if (b.reg.all_of<sim::comp::Shadow>(id))
				++n;
		});
		return n;
	};

	b.step();
	CHECK(ship(), "the ship should survive its first frame");
	CHECK(!b.collidable(shipId),
			"an arriving ship must be intangible -- that is what stops two of "
			"them materialising inside each other");
	CHECK(b.reg.all_of<comp::Collider>(shipId),
			"and it keeps its mask while intangible: WarpingIn is what makes "
			"it "
			"untouchable, not the absence of a Collider");

	// Partway through: shadows are hull-sized, not points, and -- the part
	// that was wrong twice -- each new one lands *closer* to the arrival
	// point than the last, converging onto the ship, not streaming away.
	const Vec2i arrival = b.reg.try_get<sim::comp::Position>(shipId)->current;
	const auto newestDistance = [&b, &arrival]() -> i64 {
		i32 best = -1;
		i64 dist = -1;
		b.eachOrdered([&](sim::EntityId id) {
			if (!b.reg.all_of<sim::comp::Shadow>(id))
				return;
			const i32 life = sim::framesLeft(b, id);
			if (life <= best)
				return;
			best = life;
			const Vec2i shadowAt =
					b.reg.try_get<sim::comp::Position>(id)->current;
			const Vec2i d = sim::wrapDelta(
					Vec2i{shadowAt.x - arrival.x, shadowAt.y - arrival.y});
			dist = static_cast<i64>(d.x) * d.x + static_cast<i64>(d.y) * d.y;
		});
		return dist;
	};

	int peak = 0;
	int closing = 0;
	int receding = 0;
	i64 previous = -1;
	for (int i = 0; i < sim::comp::WarpingIn::kFrames - 2; ++i)
	{
		b.step();
		peak = std::max(peak, shadows());

		const i64 d = newestDistance();
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

	sim::EntityId sId = kNoEntity;
	b.eachOrdered([&](sim::EntityId id) {
		if (sId != kNoEntity)
			return;
		if (b.reg.all_of<sim::comp::Shadow>(id))
			sId = id;
	});
	if (sId != kNoEntity)
	{
		// The shadow itself carries no Collider -- it was never collidable;
		// Draw.cpp draws it hull-sized straight from content, not from a
		// copied mask here.
		CHECK(!b.reg.all_of<sim::comp::Collider>(sId),
				"a shadow never collides, and never did");
		const sim::comp::Position &sPos =
				*b.reg.try_get<sim::comp::Position>(sId);
		CHECK(sPos.facing == Facing(4),
				"a shadow keeps the facing it was shed at");

		// And it lies *behind* the ship: spawnIonTrail offsets the exhaust
		// along facingToAngle + kHalfCircle, and that trail comes out of the
		// engines, so the same vector negated is forward.
		const int ahead = sim::facingToAngle(4);
		const Vec2i fwd{sim::cosine(ahead, 1000), sim::sine(ahead, 1000)};
		const Vec2i off = sim::wrapDelta(
				Vec2i{sPos.current.x - arrival.x, sPos.current.y - arrival.y});
		const i64 dot = static_cast<i64>(off.x) * fwd.x
				+ static_cast<i64>(off.y) * fwd.y;
		CHECK(dot < 0,
				"the trail must lie behind the ship, not ahead of it "
				"(dot=%lld, offset=%d,%d forward=%d,%d)",
				static_cast<long long>(dot), off.x, off.y, fwd.x, fwd.y);
	}

	// And it arrives: solid, still, and driving itself from here.
	for (int i = 0; i < 4; ++i)
		b.step();
	CHECK(ship(), "the ship should still be here once it arrives");
	CHECK(b.reg.all_of<sim::comp::Collider>(shipId),
			"an arrived ship must be solid");
	CHECK(!b.reg.all_of<sim::comp::WarpingIn>(shipId),
			"arrival removes the phase component");
	CHECK(b.ship(shipId)->crew == sim::earthlingCruiser().maxCrew,
			"arriving fills the crew, got %d", b.ship(shipId)->crew);
}

void testCloakHidesFromTracking()
{
	Battle b(1);
	const EntityId avenger = spawnPlayerShip(b, ilwrathAvenger(), nullptr,
			Vec2i{4000, 4000}, Facing(0), 1, /*warpIn=*/false);
	b.step();

	const EntityId hunter = spawnPlayerShip(b, earthlingCruiser(), nullptr,
			Vec2i{4000, 4400}, Facing(0), 0, /*warpIn=*/false);
	b.step();

	// Facing 8 is away from the target, so a step toward it is a real change.
	// Starting already pointed at it returns 0 quite correctly -- there is
	// nothing to turn -- which is not the same as "no target found".
	Facing facing{8};
	CHECK(trackShip(b, hunter, facing) != 0,
			"an uncloaked enemy should be trackable");

	b.reg.try_get<comp::Input>(avenger)->buttons = ShipInput::Special;
	b.step();
	b.reg.try_get<comp::Input>(avenger)->buttons = ShipInput::None;

	// Activation starts the colour walk at white; the ship is not hidden yet.
	// OBJECT_CLOAKED is STAMPFILL *and* BLACK (element.h:201-204), so the
	// whole five-colour fade stays targetable -- no unearned missile-proofing.
	CHECK(!b.reg.all_of<comp::Cloaked>(avenger),
			"activation alone must not hide the ship");
	Facing fadeFacing{8};
	CHECK(trackShip(b, hunter, fadeFacing) >= 0,
			"a half-faded ship is still targetable");

	// Five walk steps later it is black, and hidden.
	for (int i = 0; i < 5; ++i)
		b.step();
	CHECK(b.reg.all_of<comp::Cloaked>(avenger),
			"fully faded should be cloaked");
	CHECK(b.reg.all_of<comp::Cloaked>(avenger)
					== (b.reg.try_get<comp::Cloak>(avenger)->level
							== comp::Cloak::kFullLevel),
			"Cloaked must be present iff the cloak is at its full level, got "
			"level %d",
			b.reg.try_get<comp::Cloak>(avenger)->level);

	Facing cloakedFacing{8};
	CHECK(trackShip(b, hunter, cloakedFacing) < 0,
			"a cloaked ship must not be targetable at all -- TrackShip "
			"returns -1, no target (weapon.c:344-348, 410-412)");

	// It does *not* lift on its own: ilwrath.c:251-253 only unwinds the ramp
	// when SPECIAL is pressed again or the hull isn't yet fully black, so a
	// ship left alone stays hidden.
	b.reg.try_get<comp::Input>(avenger)->buttons = ShipInput::None;
	for (int i = 0; i < 40; ++i)
		b.step();
	CHECK(b.reg.all_of<comp::Cloaked>(avenger),
			"a cloak stays on until it is switched off, not until a timer "
			"runs out");

	// A second press drops it.
	b.reg.try_get<comp::Input>(avenger)->buttons = ShipInput::Special;
	b.step();
	b.reg.try_get<comp::Input>(avenger)->buttons = ShipInput::None;
	for (int i = 0; i < 20; ++i)
		b.step();
	CHECK(!b.reg.all_of<comp::Cloaked>(avenger),
			"a second press should uncloak it (ilwrath.c:251-253)");
	CHECK(b.reg.all_of<comp::Cloaked>(avenger)
					== (b.reg.try_get<comp::Cloak>(avenger)->level
							== comp::Cloak::kFullLevel),
			"Cloaked must still track the full-level line once uncloaked, got "
			"level %d",
			b.reg.try_get<comp::Cloak>(avenger)->level);

	// And firing gives you away, permanently -- the ramp runs all the way
	// back even after the trigger is released (ilwrath.c:249-252).
	b.reg.try_get<comp::Input>(avenger)->buttons = ShipInput::Special;
	b.step();
	b.reg.try_get<comp::Input>(avenger)->buttons = ShipInput::None;
	for (int i = 0; i < 20; ++i)
		b.step();
	CHECK(b.reg.all_of<comp::Cloaked>(avenger),
			"it should be hidden again before the firing check");

	b.reg.try_get<comp::Input>(avenger)->buttons = ShipInput::Weapon;
	b.step();
	b.reg.try_get<comp::Input>(avenger)->buttons = ShipInput::None;
	for (int i = 0; i < 20; ++i)
		b.step();
	CHECK(!b.reg.all_of<comp::Cloaked>(avenger),
			"firing should drop the cloak and it should not come back on its "
			"own");
}

void testCloakedFiringSnapAims()
{
	Battle b(1);
	const EntityId avenger = spawnPlayerShip(b, ilwrathAvenger(), nullptr,
			Vec2i{4000, 4000}, Facing(0), 1, /*warpIn=*/false);
	b.step();

	(void)spawnPlayerShip(b, earthlingCruiser(), nullptr, Vec2i{4400, 4000},
			Facing(0), 0, /*warpIn=*/false);
	b.step();

	// Cloak fully: activation plus the five-colour walk.
	b.reg.try_get<comp::Input>(avenger)->buttons = ShipInput::Special;
	b.step();
	b.reg.try_get<comp::Input>(avenger)->buttons = ShipInput::None;
	for (int i = 0; i < 5; ++i)
		b.step();
	CHECK(b.reg.all_of<comp::Cloaked>(avenger),
			"setup: the Avenger should be hidden");

	// Point it the wrong way, then fire from the dark.
	b.reg.try_get<comp::Position>(avenger)->facing = Facing(8);
	b.reg.try_get<comp::Input>(avenger)->buttons = ShipInput::Weapon;
	b.step();

	// The ambush snap (ilwrath.c:281-342): the discharge aims the ship at
	// the lead-predicted target before the shot leaves. Both ships are at
	// rest here, so the prediction is the target itself -- due +x, facing 4.
	CHECK(b.reg.try_get<comp::Position>(avenger)->facing == Facing(4),
			"firing from full black should snap the facing onto the target, "
			"got %d",
			b.reg.try_get<comp::Position>(avenger)->facing.raw());
	CHECK(!b.reg.all_of<comp::Cloaked>(avenger),
			"and the discharge steps the cloak off black");
	CHECK(b.ship(avenger)->specialCounter == 0,
			"and zeroes the special debounce, so re-cloak is immediate once "
			"solid (ilwrath.c:347)");
}

}  // namespace

int main()
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
	testMidFrameSpawnStaysOutOfItsOwnFrame();
	testFiniteLifeExpiresAndRunsItsDeathResponse();
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
	testTraversalOrderIsDeclared();
	testSlotReuseDoesNotReorder();
	testStaleHandlesAreDetectable();
	testTheReapKeepsTheWalkIntact();
	testEntityAddressesAreStable();
	testDeadShipBurnsAsAPhaseThenGoes();
	testShipWarpsInBeforeItIsSolid();

	if (failures != 0)
		std::printf("%d check(s) failed\n", failures);
	else
		std::printf("sim: golden RNG vectors checked at compile time; "
					"runtime checks passed\n");
	return failures != 0 ? 1 : 0;
}
