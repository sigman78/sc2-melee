// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "AniFile.hpp"

#include <charconv>

namespace uqm::content {

namespace {

bool
isSpace (char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Splits on runs of whitespace, which is what sscanf's "%s %d %d %d %d" does.
// CRLF costs nothing here because '\r' is whitespace to both.
std::vector<std::string_view>
fields (std::string_view line)
{
	std::vector<std::string_view> out;
	std::size_t i = 0;
	while (i < line.size ())
	{
		while (i < line.size () && isSpace (line[i]))
			++i;
		const std::size_t start = i;
		while (i < line.size () && !isSpace (line[i]))
			++i;
		if (i > start)
			out.push_back (line.substr (start, i - start));
	}
	return out;
}

bool
parseInt (std::string_view s, int &out)
{
	const char *begin = s.data ();
	const char *end = begin + s.size ();
	const auto [ptr, ec] = std::from_chars (begin, end, out);
	return ec == std::errc{} && ptr == end;
}

}  // namespace

AniFile
parseAni (std::string_view text, std::vector<std::string> &problems)
{
	AniFile ani;
	std::size_t lineNo = 0;

	while (!text.empty ())
	{
		++lineNo;
		const std::size_t nl = text.find ('\n');
		const std::string_view line = text.substr (0, nl);
		text = (nl == std::string_view::npos) ? std::string_view{}
											  : text.substr (nl + 1);

		const std::vector<std::string_view> f = fields (line);
		if (f.empty ())
		{
			// The C would count this as a cel and then reuse the previous
			// filename. Say so instead.
			problems.emplace_back ("line " + std::to_string (lineNo)
					+ ": blank line; the C counts this as a cel and silently "
					  "repeats the previous image");
			continue;
		}
		if (f.size () != 5)
		{
			problems.emplace_back ("line " + std::to_string (lineNo) + ": "
					+ std::to_string (f.size ())
					+ " fields, expected 5 (file transparency colormap hx hy)");
			continue;
		}

		Cel cel;
		cel.file = std::string (f[0]);

		int transparent = 0;
		if (!parseInt (f[1], transparent) || !parseInt (f[2], cel.colormapIndex)
				|| !parseInt (f[3], cel.hotspotX)
				|| !parseInt (f[4], cel.hotspotY))
		{
			problems.emplace_back ("line " + std::to_string (lineNo)
					+ ": non-numeric field in " + std::string (line));
			continue;
		}

		// gfxload.c:54-75. -1 and -2 are sentinels, not indices; 0 means
		// "index 0" on a paletted image and "black is clear" on a truecolour
		// one, which the loader cannot tell apart until the PNG is open.
		if (transparent == -1)
			cel.transparency = Transparency::None;
		else if (transparent == -2)
			cel.transparency = Transparency::PngAlpha;
		else if (transparent == 0)
		{
			cel.transparency = Transparency::BlackIsClear;
			cel.transparentIndex = 0;
		}
		else if (transparent > 0)
		{
			cel.transparency = Transparency::PaletteIndex;
			cel.transparentIndex = transparent;
		}
		else
		{
			problems.emplace_back ("line " + std::to_string (lineNo)
					+ ": transparency " + std::to_string (transparent)
					+ " is not one of -2, -1, or an index");
			continue;
		}

		ani.cels.push_back (std::move (cel));
	}

	return ani;
}

}  // namespace uqm::content
