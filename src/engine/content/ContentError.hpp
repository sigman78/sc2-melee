// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_ENGINE_CONTENT_CONTENTERROR_HPP
#define UQM2_ENGINE_CONTENT_CONTENTERROR_HPP

#include "engine/core/Types.hpp"

#include <array>
#include <string_view>

namespace uqm::content {

// Why a content file would not parse.
//
// Malformed content is a real error (docs/cpp-conventions.md rule 3): a
// human fixes the file, so it earns a report; a broken invariant is an
// assert instead.
//
// The engine branches on the code only, no prose -- `uqm2-browse` and the
// tests format one with std::format at the point it is shown, keeping ~40
// string literals out of the shipping binary.
enum class ContentErrorCode : u8
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
	u32 at = 0;        // byte offset, entry index or line number
	u64 expected = 0;
	u64 actual = 0;

	constexpr ContentError() = default;
	constexpr explicit ContentError(ContentErrorCode c, u32 at_ = 0,
			u64 expected_ = 0, u64 actual_ = 0) noexcept
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
	const auto i = static_cast<usize>(code);
	return i < kText.size() ? kText[i] : std::string_view("unknown error");
}

}  // namespace uqm::content

#endif  // UQM2_ENGINE_CONTENT_CONTENTERROR_HPP
