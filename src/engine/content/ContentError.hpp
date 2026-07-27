// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_ENGINE_CONTENT_CONTENTERROR_HPP
#define UQM2_ENGINE_CONTENT_CONTENTERROR_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace uqm::content {

// Why a content file would not parse.
//
// Malformed content is a real error rather than a bug (docs/cpp-conventions.md
// rule 3): a human has to go and fix a file, so it earns a report. Broken
// invariants -- an index past a bound the caller was told to check -- are
// asserts, and get none.
//
// What it is *not* is prose. The engine branches on the code and carries no
// message; `uqm2-browse` and the tests turn one of these into English with
// std::format at the point it is shown. That keeps ~40 string literals and
// the code that concatenates them out of the shipping binary.
enum class ContentErrorCode : std::uint8_t
{
	Empty,
	TooShort,
	Compressed,       // an LZ length prefix; unsupported since forever
	CountOverflows,   // a declared count that does not fit the file
	EntryOverruns,
	TrailingBytes,
	InvertedRange,
	WrongSize,        // right container, wrong shape for the requested one
	NoPalette,
	BadFieldCount,
	NonNumericField,
	BadTransparency,
	NotADirectory,
	DecodeFailed,     // the image codec said no
	EncodeFailed,
};

// Fixed-size, trivially copyable, no allocation. The numbers are what a
// message needs to be useful -- "entry 3 wanted 770 bytes, got 386" -- with
// the sentence itself deferred to whoever displays it.
struct ContentError
{
	ContentErrorCode code = ContentErrorCode::Empty;
	std::uint32_t at = 0;        // byte offset, entry index or line number
	std::uint64_t expected = 0;
	std::uint64_t actual = 0;

	constexpr ContentError() = default;
	constexpr explicit ContentError(ContentErrorCode c, std::uint32_t at_ = 0,
			std::uint64_t expected_ = 0, std::uint64_t actual_ = 0) noexcept
		: code(c), at(at_), expected(expected_), actual(actual_)
	{
	}

	friend constexpr bool operator==(
			const ContentError &, const ContentError &) = default;
};

// A bare noun for the code. Callers that want the numbers format them
// alongside; this deliberately does not, so it stays a constexpr table
// lookup rather than a formatting call.
[[nodiscard]] constexpr std::string_view
describe(ContentErrorCode code) noexcept
{
	constexpr std::array<std::string_view, 15> kText{
		"empty file",
		"too short",
		"LZ-compressed resource data is not supported",
		"declared entry count does not fit the file",
		"entry overruns the file",
		"trailing bytes after the last entry",
		"range start is past its end",
		"payload size does not match the requested shape",
		"no palette",
		"wrong number of fields",
		"non-numeric field",
		"unrecognised transparency value",
		"not a directory",
		"image decode failed",
		"image encode failed",
	};
	const auto i = static_cast<std::size_t>(code);
	return i < kText.size() ? kText[i] : std::string_view("unknown error");
}

}  // namespace uqm::content

#endif  // UQM2_ENGINE_CONTENT_CONTENTERROR_HPP
