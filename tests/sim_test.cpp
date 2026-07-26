// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.
//
// sim/ tests. Much of what matters here is asserted at compile time -- the
// RNG's golden vectors are `static_assert`s, so this file merely has to
// compile for them to have been checked. What is left is the behaviour that
// needs a running object: stream independence and reseed semantics.

#include "engine/core/Pacing.hpp"
#include "sim/EntityList.hpp"
#include "sim/Random.hpp"
#include "sim/World.hpp"

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

}  // namespace

int
main()
{
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
