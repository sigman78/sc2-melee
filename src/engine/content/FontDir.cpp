// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "FontDir.hpp"

#include <algorithm>
#include <system_error>

namespace uqm::content {

namespace {

// sscanf("%x.") accepts leading hex digits and stops at the first that is
// not one; it does not require the '.' it names. Reproduced rather than
// tightened, so a font directory that loads in the C loads here.
bool
parseHexPrefix (const std::string &name, std::uint32_t &out)
{
	std::uint32_t v = 0;
	std::size_t digits = 0;
	for (const char c : name)
	{
		int d;
		if (c >= '0' && c <= '9')
			d = c - '0';
		else if (c >= 'a' && c <= 'f')
			d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			d = c - 'A' + 10;
		else
			break;

		if (v > (0xFFFFFFFFu - static_cast<std::uint32_t> (d)) / 16)
			return false;  // would overflow; the C would too, quietly
		v = v * 16 + static_cast<std::uint32_t> (d);
		++digits;
	}
	if (digits == 0)
		return false;
	out = v;
	return true;
}

}  // namespace

Font
loadFontDir (const std::filesystem::path &dir, std::vector<std::string> &problems)
{
	Font font;

	std::error_code ec;
	if (!std::filesystem::is_directory (dir, ec))
	{
		problems.emplace_back (dir.string ()
				+ " is not a directory; a .fon is a directory of <hex>.png");
		return font;
	}

	for (const auto &entry : std::filesystem::directory_iterator (dir, ec))
	{
		if (!entry.is_regular_file ())
			continue;

		const std::string name = entry.path ().filename ().string ();
		std::uint32_t cp = 0;
		if (!parseHexPrefix (name, cp))
		{
			problems.emplace_back (name + ": name does not start with hex "
					"digits, so the C skips it");
			continue;
		}
		if (cp > 0xFFFF)
		{
			problems.emplace_back (name + ": code point is above 0xFFFF, so "
					"the C skips it");
			continue;
		}

		font.glyphs.push_back (Glyph{static_cast<char32_t> (cp), entry.path ()});
	}

	std::sort (font.glyphs.begin (), font.glyphs.end (),
			[] (const Glyph &a, const Glyph &b) {
				return a.codepoint < b.codepoint;
			});

	// Two files claiming the same code point means one of them never draws,
	// and which one is directory-order dependent.
	for (std::size_t i = 1; i < font.glyphs.size (); ++i)
	{
		if (font.glyphs[i].codepoint == font.glyphs[i - 1].codepoint)
		{
			problems.emplace_back (font.glyphs[i].file.filename ().string ()
					+ " and " + font.glyphs[i - 1].file.filename ().string ()
					+ " both claim the same code point");
		}
	}

	return font;
}

}  // namespace uqm::content
