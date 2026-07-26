// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_RANDOM_HPP
#define UQM2_SIM_RANDOM_HPP

#include <cstdint>

namespace uqm::sim {

// TFB_Random, bit for bit (libs/math/random.c:61-100).
//
// It looks like Park-Miller with Schrage's trick, and it is not. `seed` is a
// DWORD -- uint32 -- so when `A * (s % Q) - R * (s / Q)` goes negative it
// wraps modulo 2^32 instead of staying negative, the `seed > M` branch fires,
// and one M is subtracted. The result is `M + t + 2` where Park-Miller gives
// `M + t`:
//
//     seed          2044368000   (16000 * Q, so s % Q == 0)
//     true signed t  -45376000
//     Park-Miller   2102107647
//     TFB_Random    2102107649   <- +2
//
// So std::minstd_rand is *not* a drop-in: it agrees for the first several
// draws from many seeds and then silently diverges. The golden vectors below
// include that case on purpose, because a test that only checks seed 1 would
// pass against the wrong generator.
//
// All arithmetic is on uint32_t, where wrapping is defined. The C relies on
// the same wrap; it just gets there through implementation-defined promotions
// that happen to agree because every intermediate is non-negative and the
// products fit.
class Rng
{
public:
	static constexpr std::uint32_t kA = 16807;
	static constexpr std::uint32_t kM = 2147483647u;  // 2^31 - 1
	static constexpr std::uint32_t kQ = 127773u;      // M / A
	static constexpr std::uint32_t kR = 2836;         // M % A
	static constexpr std::uint32_t kDefaultSeed = 12345u;

	constexpr Rng() noexcept = default;
	constexpr explicit Rng(std::uint32_t seed) noexcept { reseed(seed); }

	// TFB_SeedRandom: coerces into 1..M and returns the previous seed, which
	// is how the C juggles multiple streams out of one global.
	constexpr std::uint32_t reseed(std::uint32_t seed) noexcept
	{
		if (seed == 0)
			seed = 1;
		else if (seed > kM)
			seed -= kM;

		const std::uint32_t old = seed_;
		seed_ = seed;
		return old;
	}

	[[nodiscard]] constexpr std::uint32_t seed() const noexcept
	{
		return seed_;
	}

	constexpr std::uint32_t next() noexcept
	{
		seed_ = kA * (seed_ % kQ) - kR * (seed_ / kQ);
		if (seed_ > kM)
			return (seed_ -= kM);
		if (seed_ != 0)
			return seed_;
		return (seed_ = 1);
	}

	// The 16-bit truncation the call sites apply before taking a modulo:
	// `(COUNT)TFB_Random () % 100`, `(UWORD)TFB_Random () % SPACE_WIDTH`,
	// `LOWORD (TFB_Random ())`. It is not interchangeable with next() -- the
	// truncation happens first and changes the result -- so it is a separate
	// call rather than something a helper hides.
	constexpr std::uint16_t next16() noexcept
	{
		return static_cast<std::uint16_t>(next());
	}

private:
	std::uint32_t seed_ = kDefaultSeed;
};

// --------------------------------------------------------------------------
// Golden vectors, checked at compile time.

namespace detail {

consteval bool
checkStream(std::uint32_t seed, const std::uint32_t (&want)[8]) noexcept
{
	Rng rng(seed);
	for (const std::uint32_t w : want)
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
	constexpr std::uint32_t kSeed = 127773u * 16000u;  // 2044368000
	Rng rng(kSeed);
	const std::uint32_t got = rng.next();

	// What a correct Park-Miller would have produced.
	const std::int64_t t = static_cast<std::int64_t>(Rng::kA) * (kSeed % Rng::kQ)
			- static_cast<std::int64_t>(Rng::kR) * (kSeed / Rng::kQ);
	const std::int64_t parkMiller = t + Rng::kM;

	return t < 0 && got == 2102107649u
			&& static_cast<std::int64_t>(got) - parkMiller == 2;
}
static_assert(checkParkMillerDivergence(),
		"Rng must reproduce TFB_Random's uint32 wrap, not Park-Miller");

}  // namespace detail

}  // namespace uqm::sim

#endif  // UQM2_SIM_RANDOM_HPP
