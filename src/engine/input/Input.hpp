// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_ENGINE_INPUT_INPUT_HPP
#define UQM2_ENGINE_INPUT_INPUT_HPP

#include "engine/core/Types.hpp"

namespace uqm::input {

// The input accumulator (see design-notes.md D7, V6): `held` is live state,
// `pressed` is sticky until a step consumes it, so a tap shorter than one
// battle frame (the C's per-frame poll, battle.c:198-210) still fires once.

enum class Button : u8
{
	Left,
	Right,
	Thrust,
	Weapon,
	Special,
	// BATTLE_ESCAPE: leaving the fight, not a ship action (battle.c:212-214).
	Escape,

	// Not a ship action either: toggles the collision overlay. Bound for
	// player 0 only, since it is a property of the view, not of a player.
	Debug,
};

inline constexpr usize kButtonCount = 7;

// A set of buttons. A bitset in a single word -- there are six of them, and
// this is copied per player per frame.
class Buttons
{
public:
	constexpr Buttons() = default;

	[[nodiscard]] static constexpr Buttons
	of(Button b) noexcept
	{
		return Buttons{static_cast<u32>(1u << static_cast<int>(b))};
	}

	constexpr void
	set(Button b) noexcept
	{
		bits_ |= 1u << static_cast<int>(b);
	}
	constexpr void
	clear(Button b) noexcept
	{
		bits_ &= ~(1u << static_cast<int>(b));
	}
	[[nodiscard]] constexpr bool
	test(Button b) const noexcept
	{
		return (bits_ & (1u << static_cast<int>(b))) != 0;
	}
	[[nodiscard]] constexpr bool
	any() const noexcept
	{
		return bits_ != 0;
	}

	[[nodiscard]] constexpr Buttons
	operator|(Buttons o) const noexcept
	{
		return Buttons{bits_ | o.bits_};
	}
	[[nodiscard]] constexpr Buttons
	operator&(Buttons o) const noexcept
	{
		return Buttons{bits_ & o.bits_};
	}

	friend constexpr bool operator==(Buttons, Buttons) = default;

private:
	explicit constexpr Buttons(u32 bits) noexcept : bits_(bits) {}

	u32 bits_ = 0;
};

// One player's accumulator. press/release run from the platform event pump
// as often as it likes; consume() runs exactly once per simulation step and
// is the only thing that clears the sticky half.
class InputAccumulator
{
public:
	void
	press(Button b) noexcept
	{
		held_.set(b);
		pressed_.set(b);
	}

	void
	release(Button b) noexcept
	{
		held_.clear(b);
		// pressed_ deliberately survives: a tap that started and finished
		// inside one simulation step still has to be reported by that step.
	}

	// What the step sees, and the point at which the sticky half is spent.
	[[nodiscard]] Buttons
	consume() noexcept
	{
		const Buttons out = held_ | pressed_;
		pressed_ = Buttons{};
		return out;
	}

	// What is physically down right now, without spending anything. For the
	// menu and debug paths that want a level, not an edge.
	[[nodiscard]] Buttons
	held() const noexcept
	{
		return held_;
	}

	// Losing focus, or leaving the battle. Drops the sticky half too, so a
	// press made while the window was not focused does not fire on return.
	void
	reset() noexcept
	{
		held_ = Buttons{};
		pressed_ = Buttons{};
	}

private:
	Buttons held_;
	Buttons pressed_;
};

}  // namespace uqm::input

#endif  // UQM2_ENGINE_INPUT_INPUT_HPP
