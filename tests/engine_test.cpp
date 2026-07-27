// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.
//
// engine/ tests. Presentation-adjacent machinery that is nonetheless pure
// logic and has no business needing a window to be checked.

#include "engine/core/Pacing.hpp"
#include "engine/input/Input.hpp"

#include <cstdint>
#include <cstdio>
#include <initializer_list>

namespace {

int g_failures = 0;

#define CHECK(cond, ...)                                                      \
	do                                                                        \
	{                                                                         \
		if (!(cond))                                                          \
		{                                                                     \
			++g_failures;                                                     \
			std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);         \
			std::fprintf(stderr, __VA_ARGS__);                                \
			std::fputc('\n', stderr);                                         \
		}                                                                     \
	} while (0)

using namespace uqm;
using namespace uqm::input;

// --------------------------------------------------------------------------
// The input accumulator

void
testHeldButtonsAreSeenEveryStep()
{
	InputAccumulator in;
	in.press(Button::Thrust);

	for (int step = 0; step < 5; ++step)
	{
		const Buttons b = in.consume();
		CHECK(b.test(Button::Thrust), "thrust should still be down on step %d",
				step);
	}

	in.release(Button::Thrust);
	CHECK(!in.consume().test(Button::Thrust),
			"and should stop the step after release");
}

void
testTapBetweenStepsIsNotLost()
{
	// The bug this whole class exists for. The C polls the controller once a
	// frame (battle.c:198-210), so a press and release that both land inside
	// one 42ms simulation step are invisible -- the weapon simply does not
	// fire. Here the sticky half catches it.
	InputAccumulator in;
	in.press(Button::Weapon);
	in.release(Button::Weapon);

	CHECK(in.consume().test(Button::Weapon),
			"a tap that started and ended between steps must still fire");
}

void
testTapIsSeenExactlyOnce()
{
	// The other half of the contract, and the one that is easy to lose while
	// fixing the first: a single tap must not fire twice.
	InputAccumulator in;
	in.press(Button::Weapon);
	in.release(Button::Weapon);

	CHECK(in.consume().test(Button::Weapon), "seen on the step it happened");
	CHECK(!in.consume().test(Button::Weapon), "and not again on the next");
	CHECK(!in.consume().test(Button::Weapon), "nor the one after that");
}

void
testRepeatedTapsInOneStepCollapse()
{
	// At 144Hz there are six event pumps per simulation step. Someone mashing
	// the key gets one shot per step, not six -- the step is the rate limit,
	// and the ship's own weapon cooldown is the other.
	InputAccumulator in;
	for (int i = 0; i < 6; ++i)
	{
		in.press(Button::Weapon);
		in.release(Button::Weapon);
	}

	CHECK(in.consume().test(Button::Weapon), "the mash should fire once");
	CHECK(!in.consume().test(Button::Weapon), "and not again");
}

void
testHeldSurvivesConsumeButStickyDoesNot()
{
	// A button still physically down reports through `held`, which consume()
	// must not clear -- only the sticky half is spent.
	InputAccumulator in;
	in.press(Button::Left);
	(void)in.consume();

	CHECK(in.held().test(Button::Left), "still down");
	CHECK(in.consume().test(Button::Left), "so still reported");
}

void
testResetDropsPendingInput()
{
	// Losing window focus mid-tap must not fire on return.
	InputAccumulator in;
	in.press(Button::Special);
	in.release(Button::Special);
	in.reset();

	CHECK(!in.consume().any(),
			"a reset should drop the sticky press, not bank it");
}

void
testButtonsAreIndependent()
{
	InputAccumulator in;
	in.press(Button::Left);
	in.press(Button::Weapon);
	in.release(Button::Weapon);

	const Buttons b = in.consume();
	CHECK(b.test(Button::Left) && b.test(Button::Weapon),
			"both should report on the same step");

	const Buttons after = in.consume();
	CHECK(after.test(Button::Left), "the held one persists");
	CHECK(!after.test(Button::Weapon), "the tapped one does not");
	CHECK(!after.test(Button::Thrust), "and untouched buttons stay clear");
}

// --------------------------------------------------------------------------
// Pacing

void
testStepsDueDoesNotDriftAtDisplayRates()
{
	// ONE_SECOND is 840 and the sim runs at 24Hz, so a step is 35 ticks. 840
	// divides by neither 60 nor 144, so anything that accumulates a truncated
	// per-frame delta runs slow -- 840/144 truncates to 5, which is 14% short.
	// Frame time has to be computed from the frame number.
	for (const int hz : {30, 60, 100, 144})
	{
		Pacer pacer;
		std::int64_t steps = 0;
		for (int frame = 1; frame <= hz * 4; ++frame)
			steps += pacer.stepsDue(static_cast<Ticks>(
					std::int64_t{kOneSecond} * frame / hz));

		// Four seconds of game time at 24Hz.
		CHECK(steps == 96, "at %dHz four seconds should be 96 steps, got %lld",
				hz, static_cast<long long>(steps));
	}
}

}  // namespace

int
main()
{
	testHeldButtonsAreSeenEveryStep();
	testTapBetweenStepsIsNotLost();
	testTapIsSeenExactlyOnce();
	testRepeatedTapsInOneStepCollapse();
	testHeldSurvivesConsumeButStickyDoesNot();
	testResetDropsPendingInput();
	testButtonsAreIndependent();
	testStepsDueDoesNotDriftAtDisplayRates();

	if (g_failures != 0)
	{
		std::fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	std::printf("engine: input accumulator and pacing checks passed\n");
	return 0;
}
