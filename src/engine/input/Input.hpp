// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_ENGINE_INPUT_INPUT_HPP
#define UQM2_ENGINE_INPUT_INPUT_HPP

#include <cstddef>
#include <cstdint>

namespace uqm::input {

// The input accumulator, one of M1's named forces.
//
// The C polls. battle.c:198-210 reads the controller's *current* state once
// per battle frame and builds ship_input_state from it, so a press that
// begins and ends between two frames never happened. At 24 Hz a frame is 42
// milliseconds, and a deliberate tap is comfortably shorter than that -- this
// is why firing a Cruiser missile sometimes just does not go off.
//
// It gets worse the better your hardware is. The sim runs at 24 Hz but the
// event pump runs at display rate, so on a 144 Hz screen there are six pumps
// per simulation step and five of them are thrown away.
//
// So: two bits per button rather than one. `held` is the live state, and is
// what a continuous action like thrust reads. `pressed` is sticky -- set by
// any press since the last step, cleared when the step consumes it -- and is
// what guarantees a tap is seen exactly once. Not zero times, which is the
// bug, and not twice, which would double-fire.
//
// This deliberately diverges from the C. It is a change to what the player
// feels, and the melee baseline has to be re-established with it in place
// rather than assumed to carry over.

enum class Button : std::uint8_t
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

inline constexpr std::size_t kButtonCount = 7;

// A set of buttons. A bitset in a single word -- there are six of them, and
// this is copied per player per frame.
class Buttons
{
public:
	constexpr Buttons() = default;

	[[nodiscard]] static constexpr Buttons
	of(Button b) noexcept
	{
		return Buttons{static_cast<std::uint32_t>(1u << static_cast<int>(b))};
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
	explicit constexpr Buttons(std::uint32_t bits) noexcept : bits_(bits) {}

	std::uint32_t bits_ = 0;
};

// One player's accumulator.
//
// press/release are called from the platform event pump, as often as it likes.
// consume() is called exactly once per simulation step and is the only thing
// that clears the sticky half.
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
