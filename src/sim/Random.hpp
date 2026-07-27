// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_RANDOM_HPP
#define UQM2_SIM_RANDOM_HPP

#include "engine/core/Types.hpp"

namespace uqm::sim {

// TFB_Random, bit for bit (libs/math/random.c:61-100) -- see design-notes D9.
//
//     seed          2044368000   (16000 * Q, so s % Q == 0)
//     true signed t  -45376000
//     Park-Miller   2102107647
//     TFB_Random    2102107649   <- +2
//
// All arithmetic is on uint32_t; the C gets the same wrap via promotions.
class Rng
{
public:
	static constexpr u32 kA = 16807;
	static constexpr u32 kM = 2147483647u;  // 2^31 - 1
	static constexpr u32 kQ = 127773u;      // M / A
	static constexpr u32 kR = 2836;         // M % A
	static constexpr u32 kDefaultSeed = 12345u;

	constexpr Rng() noexcept = default;
	constexpr explicit Rng(u32 seed) noexcept { reseed(seed); }

	// TFB_SeedRandom: coerces into 1..M and returns the previous seed, which
	// is how the C juggles multiple streams out of one global.
	constexpr u32 reseed(u32 seed) noexcept
	{
		if (seed == 0)
			seed = 1;
		else if (seed > kM)
			seed -= kM;

		const u32 old = seed_;
		seed_ = seed;
		return old;
	}

	[[nodiscard]] constexpr u32 seed() const noexcept
	{
		return seed_;
	}

	constexpr u32 next() noexcept
	{
		seed_ = kA * (seed_ % kQ) - kR * (seed_ / kQ);
		if (seed_ > kM)
			return (seed_ -= kM);
		if (seed_ != 0)
			return seed_;
		return (seed_ = 1);
	}

	// The 16-bit truncation call sites apply before a modulo (e.g.
	// `(COUNT)TFB_Random () % 100`). Not interchangeable with next(): the
	// truncation happens first and changes the result, so it's a separate call.
	constexpr u16 next16() noexcept
	{
		return static_cast<u16>(next());
	}

private:
	u32 seed_ = kDefaultSeed;
};

// --------------------------------------------------------------------------
// Golden vectors, checked at compile time.

namespace detail {

consteval bool
checkStream(u32 seed, const u32 (&want)[8]) noexcept
{
	Rng rng(seed);
	for (const u32 w : want)
	{
		if (rng.next() != w)
			return false;
	}
	return true;
}

// The default seed the C starts with.
static_assert(checkStream(Rng::kDefaultSeed,
		{207482415u, 1790989824u, 2035175616u, 77048696u, 24794531u,
			109854999u, 1644515420u, 1256127050u}));

// From seed 1. These first eight happen to agree with textbook minstd, which
// is exactly why the wrap case below is also asserted.
static_assert(checkStream(1u,
		{16807u, 282475249u, 1622650073u, 984943658u, 1144108930u, 470211272u,
			101027544u, 1457850878u}));

// TFB_SeedRandom coercion: 0 becomes 1, so it is the same stream.
consteval bool
checkZeroSeedIsOne() noexcept
{
	Rng a(0);
	Rng b(1);
	return a.seed() == b.seed() && a.next() == b.next();
}
static_assert(checkZeroSeedIsOne());

// ...and a seed above M has one M subtracted, so M + 5 is the seed-5 stream.
consteval bool
checkHighSeedWraps() noexcept
{
	Rng a(Rng::kM + 5u);
	Rng b(5u);
	return a.seed() == 5u && a.next() == b.next();
}
static_assert(checkHighSeedWraps());

// The case that separates this from Park-Miller. 16000 * Q leaves s % Q == 0,
// so the subtraction is negative and the uint32 wrap decides the answer.
consteval bool
checkParkMillerDivergence() noexcept
{
	constexpr u32 kSeed = 127773u * 16000u;  // 2044368000
	Rng rng(kSeed);
	const u32 got = rng.next();

	// What a correct Park-Miller would have produced.
	const i64 t = static_cast<i64>(Rng::kA) * (kSeed % Rng::kQ)
			- static_cast<i64>(Rng::kR) * (kSeed / Rng::kQ);
	const i64 parkMiller = t + Rng::kM;

	return t < 0 && got == 2102107649u
			&& static_cast<i64>(got) - parkMiller == 2;
}
static_assert(checkParkMillerDivergence(),
		"Rng must reproduce TFB_Random's uint32 wrap, not Park-Miller");

}  // namespace detail

}  // namespace uqm::sim

#endif  // UQM2_SIM_RANDOM_HPP
