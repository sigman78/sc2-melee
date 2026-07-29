// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_ENGINE_CORE_PACING_HPP
#define UQM2_ENGINE_CORE_PACING_HPP

#include "engine/core/Types.hpp"

namespace uqm {

// The game's time base. ONE_SECOND is 840 ticks (libs/timelib.h:35) -- chosen
// so the several fixed rates the game wants all divide it -- and a battle
// frame is BATTLE_FRAME_RATE == 840/24 == 35 ticks, i.e. 24 Hz, 41.667 ms.
using Ticks = i64;

inline constexpr Ticks kOneSecond = 840;
inline constexpr Ticks kBattleFrameRate = kOneSecond / 24;  // 35
inline constexpr Ticks kBattleHz = 24;

// Fixed-step accumulator: advances the deadline by a period, never resets
// it to `now` -- the naive reset form silently drops a 24 Hz step to 20 Hz
// on faster display loops. tests/engine_test.cpp asserts both rates.
//
// No wall clock read here: `now` is a parameter, so this is testable with
// synthetic time and usable from sim/ without breaking its purity rule.
class Pacer
{
public:
	// `maxSteps` bounds catch-up: an arbitrarily large `now` (a debugger
	// pause, a backgrounded tab) must not turn thousands of catch-up steps
	// into a hang, so the simulation deliberately loses that time instead.
	constexpr explicit Pacer(
			Ticks period = kBattleFrameRate, int maxSteps = 5) noexcept
		: period_(period), maxSteps_(maxSteps), dueAt_(period)
	{}

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
