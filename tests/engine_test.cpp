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
	testPacingHitsTwentyFourHertz();
	testPacingBoundsCatchUp();

	if (g_failures != 0)
	{
		std::fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	std::printf("engine: input accumulator and pacing checks passed\n");
	return 0;
}
