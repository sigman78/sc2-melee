// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "FontDir.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace uqm::content {

namespace {

// sscanf("%x.") accepts leading hex digits and stops at the first character
// that is not one; it does not require the '.' it names. Reproduced rather
// than tightened, so a font directory that loads in the C loads here.
std::optional<std::uint32_t>
parseHexPrefix(std::string_view name) noexcept
{
	std::uint32_t v = 0;
	std::size_t digits = 0;
	for (const char c : name)
	{
		std::uint32_t d;
		if (c >= '0' && c <= '9')
			d = static_cast<std::uint32_t>(c - '0');
		else if (c >= 'a' && c <= 'f')
			d = static_cast<std::uint32_t>(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			d = static_cast<std::uint32_t>(c - 'A' + 10);
		else
			break;

		if (v > (0xFFFFFFFFu - d) / 16u)
			return std::nullopt;  // would overflow; the C would too, quietly
		v = v * 16u + d;
		++digits;
	}
	return digits == 0 ? std::nullopt : std::optional<std::uint32_t>(v);
}

}  // namespace

Font
loadFontDir(const std::filesystem::path &dir,
		std::vector<ContentError> *problems)
{
	using enum ContentErrorCode;

	const auto note = [problems](ContentErrorCode code, std::uint32_t at = 0) {
		if (problems != nullptr)
			problems->emplace_back(code, at);
	};

	Font font;

	std::error_code ec;
	if (!std::filesystem::is_directory(dir, ec))
	{
		note(NotADirectory);
		return font;
	}

	for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
	{
		if (!entry.is_regular_file())
			continue;

		const std::string name = entry.path().filename().string();
		const auto cp = parseHexPrefix(name);
		if (!cp)
		{
			note(NonNumericField);
			continue;
		}
		if (*cp > 0xFFFF)
		{
			note(WrongSize, *cp);
			continue;
		}

		font.glyphs.push_back(Glyph{static_cast<char32_t>(*cp), entry.path()});
	}

	std::ranges::sort(font.glyphs, {}, &Glyph::codepoint);

	// Two files claiming the same code point means one of them never draws,
	// and which one is directory-order dependent.
	const auto dup =
			std::ranges::adjacent_find(font.glyphs, {}, &Glyph::codepoint);
	if (dup != font.glyphs.end())
		note(WrongSize, static_cast<std::uint32_t>(dup->codepoint));

	return font;
}

}  // namespace uqm::content
