// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_ENGINE_CORE_TEXT_HPP
#define UQM2_ENGINE_CORE_TEXT_HPP

#include "engine/core/Types.hpp"

#include <charconv>
#include <concepts>
#include <string_view>

namespace uqm {

// Shared text scanning (docs/cpp-conventions.md rule 8): one line splitter,
// one trim, one integer parser instead of three near-identical copies
// across content parsers. None of it allocates.

inline constexpr std::string_view kAsciiSpace = " \t\r\n";

[[nodiscard]] constexpr bool
isSpace(char c) noexcept
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

[[nodiscard]] constexpr std::string_view
trimLeft(std::string_view s) noexcept
{
	while (!s.empty() && isSpace(s.front()))
		s.remove_prefix(1);
	return s;
}

[[nodiscard]] constexpr std::string_view
trimRight(std::string_view s) noexcept
{
	while (!s.empty() && isSpace(s.back()))
		s.remove_suffix(1);
	return s;
}

[[nodiscard]] constexpr std::string_view
trim(std::string_view s) noexcept
{
	return trimLeft(trimRight(s));
}

// Splits off the next line, advancing `text`. The newline is consumed; any
// '\r' before it is left on the returned view, because content is CRLF and
// whether that matters is the caller's business, not this function's.
[[nodiscard]] constexpr std::string_view
nextLine(std::string_view &text) noexcept
{
	const usize nl = text.find('\n');
	if (nl == std::string_view::npos)
	{
		const std::string_view line = text;
		text = {};
		return line;
	}
	const std::string_view line = text.substr(0, nl);
	text = text.substr(nl + 1);
	return line;
}

// `fn(line, lineNumber)` for each line, 1-based. Taking a callable rather
// than returning a container is the whole point: no vector, no strings, and
// the loop body sits next to its only use.
template <class Fn>
constexpr void
forEachLine(std::string_view text, Fn &&fn)
{
	usize lineNo = 0;
	while (!text.empty())
	{
		++lineNo;
		fn(nextLine(text), lineNo);
	}
}

// Splits on runs of whitespace, calling `fn(field)`. Matches what
// sscanf("%s ...") does, which is what the .ani format is specified in terms
// of.
template <class Fn>
constexpr void
forEachField(std::string_view line, Fn &&fn)
{
	usize i = 0;
	while (i < line.size())
	{
		while (i < line.size() && isSpace(line[i]))
			++i;
		const usize start = i;
		while (i < line.size() && !isSpace(line[i]))
			++i;
		if (i > start)
			fn(line.substr(start, i - start));
	}
}

// Whole-view integer parse: trailing junk is a failure, not a partial
// success. std::from_chars, so no locale and no allocation.
template <class T>
	requires std::integral<T>
[[nodiscard]] inline bool
parseInt(std::string_view s, T &out) noexcept
{
	const char *begin = s.data();
	const char *end = begin + s.size();
	const auto [ptr, ec] = std::from_chars(begin, end, out);
	return ec == std::errc{} && ptr == end;
}

}  // namespace uqm

#endif  // UQM2_ENGINE_CORE_TEXT_HPP
