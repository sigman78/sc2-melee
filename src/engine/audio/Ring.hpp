// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_ENGINE_AUDIO_RING_HPP
#define UQM2_ENGINE_AUDIO_RING_HPP

#include <atomic>
#include <cstddef>
#include <span>
#include <type_traits>

namespace uqm::audio {

// A single-producer, single-consumer lock-free ring.
//
// **This is the one place in src/ that has an atomic, and it is deliberate.**
// Everything else is single-threaded and stays that way: the plan says so, the
// game loop is written as main + iterate(), and nothing else starts a thread.
// SDL's audio callback is not ours to schedule -- it runs on a device thread,
// on its own clock, and it will run while the simulation is mid-step. There
// are three ways to meet that and only one of them is any good:
//
//   - lock the game state from the callback: now the audio device can stall
//     on a mutex held by a physics step, and an underrun is an audible click;
//   - copy the game state for the callback: the copy still has to be
//     synchronised, so this is the same problem with more memory;
//   - never let the callback see game state at all. The main loop renders
//     audio into this ring, the callback drains it and does nothing else.
//
// The third is what this is for. The invariant is that the consumer touches
// exactly two things -- this buffer and its own head index -- so "is the
// simulation single-threaded" stays answerable with "yes", and the boundary is
// one small file with tests rather than a rule everyone has to remember.
//
// Capacity must be a power of two. Indices grow without wrapping and are
// masked on access, which is what removes the classic full-versus-empty
// ambiguity: `tail - head` is the exact count and needs no spare slot.
template <class T, std::size_t Capacity>
class SpscRing
{
	static_assert(Capacity != 0 && (Capacity & (Capacity - 1)) == 0,
			"Capacity must be a power of two");
	static_assert(std::is_trivially_copyable_v<T>,
			"a value crossing this boundary must be safe to memcpy");

public:
	[[nodiscard]] static constexpr std::size_t
	capacity() noexcept
	{
		return Capacity;
	}

	// Producer side. Writes all of `in` or none of it, and reports which --
	// a half-written buffer of audio is a click, so partial writes are not an
	// improvement over a dropped one.
	[[nodiscard]] bool
	push(std::span<const T> in) noexcept
	{
		const std::size_t tail = tail_.load(std::memory_order_relaxed);
		const std::size_t head = head_.load(std::memory_order_acquire);
		if (in.size() > Capacity - (tail - head))
			return false;

		for (std::size_t i = 0; i < in.size(); ++i)
			data_[(tail + i) & kMask] = in[i];

		// Release: everything written above must be visible to the consumer
		// before it can see the index that makes it readable.
		tail_.store(tail + in.size(), std::memory_order_release);
		return true;
	}

	// Consumer side. Takes as much as is there, up to `out.size()`, and
	// returns how much. A short read is normal and the caller pads with
	// silence -- that is a quieter failure than blocking a device thread.
	[[nodiscard]] std::size_t
	pop(std::span<T> out) noexcept
	{
		const std::size_t head = head_.load(std::memory_order_relaxed);
		const std::size_t tail = tail_.load(std::memory_order_acquire);

		std::size_t n = tail - head;
		if (n > out.size())
			n = out.size();

		for (std::size_t i = 0; i < n; ++i)
			out[i] = data_[(head + i) & kMask];

		head_.store(head + n, std::memory_order_release);
		return n;
	}

	// Approximate by nature: on the producer side it is a lower bound on what
	// the consumer will find, and vice versa. Good enough to decide whether to
	// render more audio this frame, which is all it is for.
	[[nodiscard]] std::size_t
	size() const noexcept
	{
		return tail_.load(std::memory_order_acquire)
				- head_.load(std::memory_order_acquire);
	}

	[[nodiscard]] std::size_t
	space() const noexcept
	{
		return Capacity - size();
	}

	[[nodiscard]] bool
	empty() const noexcept
	{
		return size() == 0;
	}

private:
	static constexpr std::size_t kMask = Capacity - 1;

	// Written by the consumer, read by the producer, and vice versa. Kept
	// apart so the two do not share a cache line and fight over it.
	alignas(64) std::atomic<std::size_t> head_{0};
	alignas(64) std::atomic<std::size_t> tail_{0};
	alignas(64) T data_[Capacity]{};
};

}  // namespace uqm::audio

#endif  // UQM2_ENGINE_AUDIO_RING_HPP
