// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.
//
// engine/ and game/ tests. Presentation-adjacent machinery that is
// nonetheless pure logic and has no business needing a window to be checked --
// the input accumulator, the pacing accumulator, and the melee camera.

#include "engine/core/Pacing.hpp"
#include "engine/core/Types.hpp"
#include "engine/input/Input.hpp"
#include "game/Camera.hpp"

#include <array>
#include <cstdio>
#include <initializer_list>

namespace {

int g_failures = 0;

#define CHECK(cond, ...)                                                       \
	do                                                                         \
	{                                                                          \
		if (!(cond))                                                           \
		{                                                                      \
			++g_failures;                                                      \
			std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);          \
			std::fprintf(stderr, __VA_ARGS__);                                 \
			std::fputc('\n', stderr);                                          \
		}                                                                      \
	} while (0)

using namespace uqm;
using namespace uqm::input;

// --------------------------------------------------------------------------
// The input accumulator

void testHeldButtonsAreSeenEveryStep()
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

void testTapBetweenStepsIsNotLost()
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

void testTapIsSeenExactlyOnce()
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

void testRepeatedTapsInOneStepCollapse()
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

void testHeldSurvivesConsumeButStickyDoesNot()
{
	// A button still physically down reports through `held`, which consume()
	// must not clear -- only the sticky half is spent.
	InputAccumulator in;
	in.press(Button::Left);
	(void)in.consume();

	CHECK(in.held().test(Button::Left), "still down");
	CHECK(in.consume().test(Button::Left), "so still reported");
}

void testResetDropsPendingInput()
{
	// Losing window focus mid-tap must not fire on return.
	InputAccumulator in;
	in.press(Button::Special);
	in.release(Button::Special);
	in.reset();

	CHECK(!in.consume().any(),
			"a reset should drop the sticky press, not bank it");
}

void testButtonsAreIndependent()
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
constexpr uqm::Ticks frameTime(int f, int fps)
{
	return uqm::kOneSecond * f / fps;
}

// Runs `seconds` of display frames at `fps` and counts simulation steps, the
// way the real loop would.
constexpr int runPacer(uqm::Pacer &pacer, int fps, int seconds)
{
	int steps = 0;
	for (int f = 1; f <= fps * seconds; ++f)
		steps += pacer.stepsDue(frameTime(f, fps));
	return steps;
}

// The naive form the plan warns about: deadline reset to `now` rather than
// advanced by a period. Reproduced here so the test can show what it costs.
constexpr int runNaive(uqm::Ticks period, int fps, int seconds)
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

void testPacingHitsTwentyFourHertz()
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

void testPacingBoundsCatchUp()
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
// The melee camera

void testCameraCentresBetweenTheShips()
{
	using namespace uqm::game;

	Camera cam;
	const std::array<Vec2i, 2> ships{
			Vec2i{sim::kArena.w / 2 - 512, sim::kArena.h / 2},
			Vec2i{sim::kArena.w / 2 + 512, sim::kArena.h / 2}};
	cam.follow(ships);

	CHECK(cam.centre().x == sim::kArena.w / 2,
			"the view should sit midway between them, got %ld",
			static_cast<long>(cam.centre().x));

	// Symmetric about the centre of the viewport.
	const Vec2i a = cam.toScreen(ships[0]);
	const Vec2i b = cam.toScreen(ships[1]);
	CHECK(sim::kSpace.w / 2 - a.x == b.x - sim::kSpace.w / 2,
			"both ships should be equidistant from the middle, got %ld and %ld",
			static_cast<long>(a.x), static_cast<long>(b.x));
	CHECK(a.y == sim::kSpace.h / 2 && b.y == sim::kSpace.h / 2,
			"and level with each other");
}

void testCameraZoomsOutAsShipsSeparate()
{
	using namespace uqm::game;

	i32 previous = 0;
	for (const i32 gap : {256, 1024, 2048, 4096})
	{
		Camera cam;
		const std::array<Vec2i, 2> ships{
				Vec2i{sim::kArena.w / 2 - gap / 2, sim::kArena.h / 2},
				Vec2i{sim::kArena.w / 2 + gap / 2, sim::kArena.h / 2}};
		cam.follow(ships);

		CHECK(cam.zoom() >= previous,
				"zoom must not decrease as the gap grows to %ld, got %ld "
				"after %ld",
				static_cast<long>(gap), static_cast<long>(cam.zoom()),
				static_cast<long>(previous));
		CHECK(cam.zoom() >= kZoomOne && cam.zoom() <= kMaxZoomOut,
				"zoom %ld should stay within 1:1 and 4:1",
				static_cast<long>(cam.zoom()));
		previous = cam.zoom();
	}
	CHECK(previous == kMaxZoomOut,
			"a quarter-arena gap should be fully zoomed out, got %ld",
			static_cast<long>(previous));
}

void testCameraKeepsBothShipsOnScreen()
{
	using namespace uqm::game;

	// The property that actually matters, and the one a zoom bug breaks: at
	// every separation the camera can be asked about, both ships are inside
	// the viewport.
	for (i32 gap = 0; gap <= 4096; gap += 64)
	{
		Camera cam;
		const std::array<Vec2i, 2> ships{
				Vec2i{sim::kArena.w / 2 - gap / 2, sim::kArena.h / 2},
				Vec2i{sim::kArena.w / 2 + gap / 2, sim::kArena.h / 2}};
		cam.follow(ships);

		for (const Vec2i &s : ships)
		{
			const Vec2i p = cam.toScreen(s);
			CHECK(p.x >= 0 && p.x <= sim::kSpace.w,
					"gap %ld put a ship at x=%ld, outside 0..%ld",
					static_cast<long>(gap), static_cast<long>(p.x),
					static_cast<long>(sim::kSpace.w));
		}
	}
}

void testCameraDoesNotJitter()
{
	using namespace uqm::game;

	// A ship crossing the field at a constant speed must not appear to stutter.
	// Two things used to make it: `scale` converted world to display and *then*
	// divided by the zoom, truncating twice, and the origin was snapped to a
	// four-world-unit grid -- which at 1:1 is a whole pixel, applied to every
	// object at once, so the entire screen judders as the midpoint crosses a
	// boundary.
	//
	// The property that catches both: with the view held still, equal steps in
	// world space must produce screen positions that never go backwards, and
	// whose steps never differ by more than one pixel.
	// The view is placed once and then held, so this isolates the projection
	// from the zoom. Re-following every frame is *also* jittery, but for a
	// different and inherent reason -- see testContinuousZoomRescalesEveryFrame
	// below -- and mixing the two makes neither diagnosable.
	//
	// Two ships in the follow, so the midpoint arithmetic runs; with one ship
	// the centre is simply that ship and the origin path is never exercised.
	Camera cam;
	constexpr i32 kStep = 7;  // deliberately not a multiple of 4
	const Vec2i anchor{sim::kArena.w / 2, sim::kArena.h / 2};
	const std::array<Vec2i, 2> placed{
			Vec2i{anchor.x - 256, anchor.y}, Vec2i{anchor.x + 256, anchor.y}};
	cam.follow(placed);

	i32 previous = 0;
	i32 previousDelta = -1;
	for (int frame = 1; frame <= 200; ++frame)
	{
		const Vec2i at =
				cam.toScreen(Vec2i{anchor.x + kStep * frame, anchor.y});
		if (frame > 1)
		{
			const i32 delta = at.x - previous;
			CHECK(delta >= 0, "frame %d moved backwards by %ld", frame,
					static_cast<long>(-delta));
			if (previousDelta >= 0)
			{
				const i32 wobble = delta > previousDelta
						? delta - previousDelta
						: previousDelta - delta;
				CHECK(wobble <= 1,
						"frame %d stepped %ld after %ld -- uneven motion is "
						"what jitter looks like",
						frame, static_cast<long>(delta),
						static_cast<long>(previousDelta));
			}
			previousDelta = delta;
		}
		previous = at.x;
	}
}

void testContinuousZoomRescalesEveryFrame()
{
	using namespace uqm::game;

	// Not a bug, but worth pinning because it is what "the continuous mode
	// looks shimmery" actually is. Recomputing a continuous zoom from a
	// separation that changes every frame means every sprite is rescaled by a
	// slightly different ratio every frame, so edges crawl. The stepped mode
	// exists precisely because it does not do this: it holds one of three
	// power-of-two zooms, where every sprite pixel lands on a screen pixel.
	//
	// If this ever stops being true -- if the zoom is smoothed or quantised --
	// this test should be updated deliberately, not deleted in passing.
	int changes = 0;
	i32 last = 0;
	for (int frame = 1; frame <= 60; ++frame)
	{
		Camera cam;
		const i32 gap = 600 + 7 * frame;
		const std::array<Vec2i, 2> ships{
				Vec2i{sim::kArena.w / 2 - gap / 2, sim::kArena.h / 2},
				Vec2i{sim::kArena.w / 2 + gap / 2, sim::kArena.h / 2}};
		cam.follow(ships);
		if (frame > 1 && cam.zoom() != last)
			++changes;
		last = cam.zoom();
	}

	if (kMeleeScale == MeleeScale::Continuous)
		CHECK(changes > 30,
				"a continuous zoom should change most frames as the ships "
				"separate, got %d in 60 -- if this dropped, the zoom is being "
				"smoothed and the comment above is stale",
				changes);
	else
		CHECK(changes <= 2, "a stepped zoom should hold, got %d changes",
				changes);
}

void testCameraMeasuresAcrossTheSeam()
{
	using namespace uqm::game;

	// Two ships either side of the wrap are close together, not an arena
	// apart. Without wrapDelta the camera would zoom fully out and place them
	// on opposite edges, which is the visible symptom of forgetting the torus.
	Camera cam;
	const std::array<Vec2i, 2> ships{Vec2i{16, sim::kArena.h / 2},
			Vec2i{sim::kArena.w - 240, sim::kArena.h / 2}};
	cam.follow(ships);

	CHECK(cam.zoom() == kZoomOne,
			"256 units apart across the seam should not zoom out, got %ld",
			static_cast<long>(cam.zoom()));

	const Vec2i a = cam.toScreen(ships[0]);
	const Vec2i b = cam.toScreen(ships[1]);
	const i32 apart = a.x > b.x ? a.x - b.x : b.x - a.x;
	CHECK(apart == sim::worldToDisplay(256),
			"they should be %ld pixels apart on screen, got %ld",
			static_cast<long>(sim::worldToDisplay(256)),
			static_cast<long>(apart));
}

}  // namespace

int main()
{
	testCameraCentresBetweenTheShips();
	testCameraZoomsOutAsShipsSeparate();
	testCameraKeepsBothShipsOnScreen();
	testCameraMeasuresAcrossTheSeam();
	testCameraDoesNotJitter();
	testContinuousZoomRescalesEveryFrame();
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
	std::printf("engine: input, pacing and camera checks passed\n");
	return 0;
}
