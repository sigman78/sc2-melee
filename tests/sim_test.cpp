// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.
//
// sim/ tests. Much of what matters here is asserted at compile time -- the
// RNG's golden vectors are `static_assert`s, so this file merely has to
// compile for them to have been checked. What is left is the behaviour that
// needs a running object: stream independence and reseed semantics.

#include "sim/Random.hpp"

#include <cstdio>

using namespace uqm::sim;

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

}  // namespace

int
main()
{
	testStreamsAreIndependent();
	testReseedReturnsThePrevious();
	testTruncationWidthMatters();

	if (failures != 0)
		std::printf("%d check(s) failed\n", failures);
	else
		std::printf("sim: golden RNG vectors checked at compile time; "
					"runtime checks passed\n");
	return failures != 0 ? 1 : 0;
}
