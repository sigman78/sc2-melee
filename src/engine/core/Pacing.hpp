// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_ENGINE_CORE_PACING_HPP
#define UQM2_ENGINE_CORE_PACING_HPP

#include <cstdint>

namespace uqm {

// The game's time base. ONE_SECOND is 840 ticks (libs/timelib.h:35) -- chosen
// so the several fixed rates the game wants all divide it -- and a battle
// frame is BATTLE_FRAME_RATE == 840/24 == 35 ticks, i.e. 24 Hz, 41.667 ms.
using Ticks = std::int64_t;

inline constexpr Ticks kOneSecond = 840;
inline constexpr Ticks kBattleFrameRate = kOneSecond / 24;  // 35
inline constexpr Ticks kBattleHz = 24;

// A fixed-step accumulator: how many simulation steps are owed at `now`.
//
// The whole reason this is a type with a test rather than three lines in the
// main loop is the failure the plan calls out. The naive form
//
//     if (now >= next + period) { step(); next = now; }
//
// resets the deadline to *now* rather than advancing it by a period, so it
// cannot run a step until a full period has elapsed since the last one --
// and on a display-rate loop that means waiting for the next whole frame.
// At 60 Hz a frame is 14 ticks and a battle step is 35, so steps land on
// ticks 42, 84, 126: every 42 ticks, which is 20 Hz, not 24. A 17% global
// slowdown, browser-only, and invisible on desktop because nothing in the
// tree vsyncs. tests/engine_test.cpp asserts both rates so the naive form
// cannot come back.
//
// No wall clock is read here: `now` is a parameter, so this is testable with
// synthetic time and usable from sim/ without breaking its purity rule.
class Pacer
{
public:
	// `maxSteps` bounds catch-up. A debugger pause or a backgrounded tab can
	// hand back an arbitrarily large `now`, and running thousands of steps to
	// "catch up" turns a hitch into a hang; the simulation deliberately loses
	// that time instead.
	constexpr explicit Pacer(
			Ticks period = kBattleFrameRate, int maxSteps = 5) noexcept
		: period_(period), maxSteps_(maxSteps), dueAt_(period)
	{
	}

	// Restart the cadence at `now`. Use after a pause, a mode change, or any
	// gap that should not be caught up.
	constexpr void reset(Ticks now) noexcept { dueAt_ = now + period_; }

	[[nodiscard]] constexpr Ticks period() const noexcept { return period_; }
	[[nodiscard]] constexpr Ticks dueAt() const noexcept { return dueAt_; }

	// Steps owed at `now`, advancing the deadline by one period each -- never
	// to `now`, which is the bug above. Returns 0 most display frames.
	[[nodiscard]] constexpr int stepsDue(Ticks now) noexcept
	{
		int steps = 0;
		while (now >= dueAt_)
		{
			if (steps == maxSteps_)
			{
				// Too far behind to catch up. Drop the backlog and resync,
				// rather than accumulating a debt that never clears.
				dueAt_ = now + period_;
				return steps;
			}
			dueAt_ += period_;
			++steps;
		}
		return steps;
	}

private:
	Ticks period_;
	int maxSteps_;
	Ticks dueAt_;
};

}  // namespace uqm

#endif  // UQM2_ENGINE_CORE_PACING_HPP
