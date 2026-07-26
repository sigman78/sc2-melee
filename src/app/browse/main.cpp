// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.
//
// uqm2-browse -- a content browser for the formats in
// docs/content-formats.md.
//
// The plan (docs/game-rewrite-plan.md, M0) asks for a sprite/font/colormap
// browser on the grounds that "every byte-level surprise in the project lives
// here and this is the cheapest place to find it". This is that, as a CLI
// that writes PNG contact sheets rather than a window: it needs no platform
// layer (there isn't one yet), it runs headless, and its output is a file you
// can put in a bug report.
//
// The sprite sheets are the interesting part. A UQM sprite is an indexed PNG
// whose *displayed* colours come from a colormap slot named in the .ani and
// supplied by a .ct -- supox.ani says slot 10, supox.ct declares 10..10 --
// and nothing in the tree checks that those two agree. Rendering a sheet
// through the colormap makes that binding visible, and makes a broken one
// obvious instead of subtle.

#include "engine/content/AniFile.hpp"
#include "engine/content/BinaryTable.hpp"
#include "engine/content/ColorTable.hpp"
#include "engine/content/FontDir.hpp"
#include "engine/content/PhraseFile.hpp"
#include "engine/content/PngImage.hpp"
#include "engine/content/ResourceMap.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace uqm::content;
namespace fs = std::filesystem;

namespace {

// --------------------------------------------------------------------------
// Plumbing

std::vector<std::byte>
readFile (const fs::path &p)
{
	std::ifstream in (p, std::ios::binary);
	if (!in)
		return {};
	const std::string s ((std::istreambuf_iterator<char> (in)),
			std::istreambuf_iterator<char> ());
	std::vector<std::byte> out (s.size ());
	std::memcpy (out.data (), s.data (), s.size ());
	return out;
}

std::string
readText (const fs::path &p)
{
	const std::vector<std::byte> b = readFile (p);
	return std::string (reinterpret_cast<const char *> (b.data ()), b.size ());
}

bool
writeFile (const fs::path &p, const std::vector<std::byte> &bytes)
{
	std::ofstream out (p, std::ios::binary);
	if (!out)
		return false;
	out.write (reinterpret_cast<const char *> (bytes.data ()),
			static_cast<std::streamsize> (bytes.size ()));
	return out.good ();
}

// A plain RGBA canvas. Deliberately not an engine type: the browser draws for
// human eyes, and none of this belongs anywhere near the renderer.
struct Canvas
{
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::vector<std::uint8_t> px;

	Canvas (std::uint32_t w, std::uint32_t h)
		: width (w), height (h), px (std::size_t{w} * h * 4, 0)
	{
	}

	void
	set (std::uint32_t x, std::uint32_t y, Rgb c, std::uint8_t a = 255)
	{
		if (x >= width || y >= height)
			return;
		const std::size_t i = (std::size_t{y} * width + x) * 4;
		px[i + 0] = c.r;
		px[i + 1] = c.g;
		px[i + 2] = c.b;
		px[i + 3] = a;
	}

	void
	fill (Rgb c, std::uint8_t a = 255)
	{
		for (std::uint32_t y = 0; y < height; ++y)
			for (std::uint32_t x = 0; x < width; ++x)
				set (x, y, c, a);
	}

	// Transparency has to be visible, not merely absent -- "this cel has a
	// hole in it" and "this cel failed to load" look identical on black.
	void
	checkerboard (std::uint32_t cell = 8)
	{
		for (std::uint32_t y = 0; y < height; ++y)
		{
			for (std::uint32_t x = 0; x < width; ++x)
			{
				const bool dark = ((x / cell) + (y / cell)) % 2 == 0;
				set (x, y, dark ? Rgb{40, 40, 46} : Rgb{58, 58, 66});
			}
		}
	}

	void
	rect (std::uint32_t x0, std::uint32_t y0, std::uint32_t w, std::uint32_t h,
			Rgb c)
	{
		for (std::uint32_t x = x0; x < x0 + w; ++x)
		{
			set (x, y0, c);
			set (x, y0 + h - 1, c);
		}
		for (std::uint32_t y = y0; y < y0 + h; ++y)
		{
			set (x0, y, c);
			set (x0 + w - 1, y, c);
		}
	}

	void
	crosshair (std::uint32_t cx, std::uint32_t cy, Rgb c, std::uint32_t arm = 3)
	{
		for (std::uint32_t d = 0; d <= arm; ++d)
		{
			set (cx + d, cy, c);
			set (cx - d, cy, c);
			set (cx, cy + d, c);
			set (cx, cy - d, c);
		}
	}
};

// --------------------------------------------------------------------------
// Colormaps

// Slot -> palette, built from a .ct read in the Palettes shape. A .ct entry
// covers a *range* of slots, so one entry can populate many.
using SlotMap = std::map<int, Palette>;

SlotMap
loadColormaps (const fs::path &ct, std::vector<std::string> &problems)
{
	SlotMap slots;
	const std::vector<std::byte> bytes = readFile (ct);
	if (bytes.empty ())
	{
		problems.push_back ("cannot read " + ct.string ());
		return slots;
	}

	std::string err;
	const auto table = parseBinaryTable (bytes, err);
	if (!table)
	{
		problems.push_back (ct.filename ().string () + ": " + err);
		return slots;
	}

	for (std::size_t i = 0; i < table->entries.size (); ++i)
	{
		std::string entryErr;
		const auto entry = parseColorTableEntry (
				table->entries[i], ColorTableShape::Palettes, entryErr);
		if (!entry)
		{
			// Very likely a partial-palette table (a planets/*.ct). Not an
			// error, just not something a sprite sheet can colour with.
			problems.push_back (ct.filename ().string () + " entry "
					+ std::to_string (i)
					+ " is not a full-palette table: " + entryErr);
			continue;
		}
		for (std::size_t p = 0; p < entry->palettes.size (); ++p)
			slots[entry->first + static_cast<int> (p)] = entry->palettes[p];
	}
	return slots;
}

// --------------------------------------------------------------------------
// Drawing content

// Draws one decoded image into a canvas at (ox, oy). Indexed images are
// coloured through `palette` when one is supplied -- that is the whole point
// of the sheet -- and fall back to the PNG's own PLTE when it is not.
void
blit (Canvas &c, const PngImage &img, std::uint32_t ox, std::uint32_t oy,
		const Palette *palette)
{
	for (std::uint32_t y = 0; y < img.height; ++y)
	{
		for (std::uint32_t x = 0; x < img.width; ++x)
		{
			if (img.format == PixelFormat::Rgba8)
			{
				const std::size_t i = (std::size_t{y} * img.width + x) * 4;
				if (img.pixels[i + 3] == 0)
					continue;
				c.set (ox + x, oy + y,
						Rgb{img.pixels[i], img.pixels[i + 1],
							img.pixels[i + 2]},
						img.pixels[i + 3]);
			}
			else
			{
				const std::uint8_t idx =
						img.pixels[std::size_t{y} * img.width + x];
				if (img.transparentIndex >= 0
						&& idx == img.transparentIndex)
					continue;
				Rgb col{255, 0, 255};  // magenta: index with no colour
				if (palette)
					col = (*palette)[idx];
				else if (idx < img.palette.size ())
					col = img.palette[idx];
				c.set (ox + x, oy + y, col);
			}
		}
	}
}

struct Sheet
{
	Canvas canvas;
	std::uint32_t cellW = 0;
	std::uint32_t cellH = 0;
	std::uint32_t cols = 0;
};

Sheet
makeSheet (std::size_t count, std::uint32_t maxW, std::uint32_t maxH)
{
	const std::uint32_t pad = 4;
	const std::uint32_t cellW = maxW + pad * 2;
	const std::uint32_t cellH = maxH + pad * 2;
	const auto cols = static_cast<std::uint32_t> (
			std::max<std::size_t> (1, static_cast<std::size_t> (
					std::ceil (std::sqrt (static_cast<double> (count))))));
	const auto rows = static_cast<std::uint32_t> ((count + cols - 1) / cols);

	Sheet s{Canvas (cols * cellW, std::max (rows, 1u) * cellH), cellW, cellH,
		cols};
	s.canvas.checkerboard ();
	return s;
}

// --------------------------------------------------------------------------
// Commands

int
cmdInventory (const fs::path &content)
{
	std::vector<std::string> problems;
	const ResourceMap map =
			ResourceMap::parse (readText (content / "uqm.rmp"), problems);

	std::map<std::string, std::size_t> byType;
	std::size_t missing = 0;
	for (const auto &[key, res] : map.entries ())
	{
		++byType[res.type];

		// Not every resource value is a path. A SHIP value is an index into
		// a table compiled into the binary -- "ship.supox.code = SHIP:16" --
		// because a ship's code is code, not content. Checking those for
		// existence reports all 28 of them as dangling, which is how this
		// exception was found in the first place.
		if (res.type == "SHIP")
			continue;

		if (!fs::exists (content / res.path))
		{
			std::printf ("  dangling: %s -> %s\n", key.c_str (),
					res.path.c_str ());
			++missing;
		}
	}

	std::printf ("%zu resources in uqm.rmp\n", map.size ());
	for (const auto &[type, n] : byType)
		std::printf ("  %-14s %zu\n", type.c_str (), n);
	std::printf ("  %zu dangling\n", missing);

	// Orphans: files present in the tree that no resource key names. Some are
	// legitimate (the .png files an .ani lists are reached through it, not
	// through the map), so this reports rather than complains.
	std::size_t files = 0;
	for (const auto &e : fs::recursive_directory_iterator (content))
		if (e.is_regular_file ())
			++files;
	std::printf ("  %zu files in the tree\n", files);

	for (const std::string &p : problems)
		std::printf ("  rmp: %s\n", p.c_str ());
	return problems.empty () && missing == 0 ? 0 : 1;
}

int
cmdAni (const fs::path &aniPath, const fs::path &out, const fs::path &ctPath)
{
	std::vector<std::string> problems;
	const AniFile ani = parseAni (readText (aniPath), problems);
	for (const std::string &p : problems)
		std::printf ("  %s\n", p.c_str ());
	if (ani.cels.empty ())
	{
		std::printf ("no cels in %s\n", aniPath.string ().c_str ());
		return 1;
	}

	SlotMap slots;
	if (!ctPath.empty ())
	{
		std::vector<std::string> ctProblems;
		slots = loadColormaps (ctPath, ctProblems);
		for (const std::string &p : ctProblems)
			std::printf ("  %s\n", p.c_str ());
	}

	// Decode everything first: the sheet's cell size is the largest cel.
	std::vector<PngImage> images;
	std::uint32_t maxW = 1, maxH = 1;
	for (const Cel &cel : ani.cels)
	{
		const fs::path png = aniPath.parent_path () / cel.file;
		std::string err;
		const auto img = decodePng (readFile (png), err);
		if (!img)
		{
			std::printf ("  %s: %s\n", cel.file.c_str (), err.c_str ());
			images.emplace_back ();
			continue;
		}
		maxW = std::max (maxW, img->width);
		maxH = std::max (maxH, img->height);
		images.push_back (*img);
	}

	Sheet sheet = makeSheet (images.size (), maxW, maxH);
	std::size_t missingSlots = 0;
	for (std::size_t i = 0; i < images.size (); ++i)
	{
		const std::uint32_t cx =
				static_cast<std::uint32_t> (i % sheet.cols) * sheet.cellW;
		const std::uint32_t cy =
				static_cast<std::uint32_t> (i / sheet.cols) * sheet.cellH;

		const Palette *pal = nullptr;
		if (!slots.empty () && images[i].format == PixelFormat::Indexed8)
		{
			const auto it = slots.find (ani.cels[i].colormapIndex);
			if (it != slots.end ())
				pal = &it->second;
			else
				++missingSlots;
		}

		blit (sheet.canvas, images[i], cx + 4, cy + 4, pal);

		// The hotspot is the cel's origin, and it is routinely negative --
		// supox-001 is at (-81, -30). Drawn where it falls, clipped by the
		// canvas when it falls outside, because "the hotspot is off the cel"
		// is a real and visible property.
		sheet.canvas.crosshair (
				static_cast<std::uint32_t> (
						static_cast<int> (cx + 4) - ani.cels[i].hotspotX),
				static_cast<std::uint32_t> (
						static_cast<int> (cy + 4) - ani.cels[i].hotspotY),
				Rgb{255, 220, 0});
		sheet.canvas.rect (cx, cy, sheet.cellW, sheet.cellH, Rgb{90, 90, 100});
	}

	if (missingSlots)
	{
		std::printf ("  %zu cel(s) name a colormap slot the .ct does not "
					 "supply\n", missingSlots);
	}

	std::string err;
	const auto png = encodeRgbaPng (sheet.canvas.width, sheet.canvas.height,
			sheet.canvas.px, err);
	if (!png || !writeFile (out, *png))
	{
		std::printf ("could not write %s: %s\n", out.string ().c_str (),
				err.c_str ());
		return 1;
	}
	std::printf ("%s: %zu cels -> %s (%ux%u)%s\n",
			aniPath.filename ().string ().c_str (), ani.cels.size (),
			out.string ().c_str (), sheet.canvas.width, sheet.canvas.height,
			slots.empty () ? " [PNG palettes]" : " [colormapped]");
	return 0;
}

int
cmdCt (const fs::path &ctPath, const fs::path &out)
{
	const std::vector<std::byte> bytes = readFile (ctPath);
	std::string err;
	const auto table = parseBinaryTable (bytes, err);
	if (!table)
	{
		std::printf ("%s: %s\n", ctPath.string ().c_str (), err.c_str ());
		return 1;
	}

	// Try each entry as both shapes and say which it is -- the ambiguity is
	// the headline fact about this format, so the browser should show it
	// rather than hide it behind a guess.
	struct Row
	{
		std::vector<Rgb> colors;
		std::string label;
	};
	std::vector<Row> rows;

	for (std::size_t i = 0; i < table->entries.size (); ++i)
	{
		std::string errA, errB;
		if (const auto a = parseColorTableEntry (
					table->entries[i], ColorTableShape::Palettes, errA))
		{
			for (std::size_t p = 0; p < a->palettes.size (); ++p)
			{
				Row row;
				row.colors.assign (a->palettes[p].begin (), a->palettes[p].end ());
				row.label = "entry " + std::to_string (i) + " slot "
						+ std::to_string (a->first + static_cast<int> (p));
				rows.push_back (std::move (row));
			}
		}
		else if (const auto b = parseColorTableEntry (
						 table->entries[i], ColorTableShape::PartialPalette,
						 errB))
		{
			Row row;
			row.colors = b->colors;
			row.label = "entry " + std::to_string (i) + " indices "
					+ std::to_string (b->first) + ".." + std::to_string (b->last);
			rows.push_back (std::move (row));
		}
		else
		{
			std::printf ("  entry %zu is neither shape (%s / %s)\n", i,
					errA.c_str (), errB.c_str ());
		}
	}

	if (rows.empty ())
		return 1;

	const std::uint32_t swatch = 8;
	std::uint32_t widest = 0;
	for (const Row &r : rows)
		widest = std::max (widest,
				static_cast<std::uint32_t> (r.colors.size ()));

	Canvas canvas (widest * swatch, static_cast<std::uint32_t> (rows.size ())
			* swatch);
	canvas.fill (Rgb{20, 20, 24});
	for (std::size_t r = 0; r < rows.size (); ++r)
	{
		for (std::size_t c = 0; c < rows[r].colors.size (); ++c)
		{
			for (std::uint32_t dy = 0; dy < swatch; ++dy)
				for (std::uint32_t dx = 0; dx < swatch; ++dx)
					canvas.set (
							static_cast<std::uint32_t> (c) * swatch + dx,
							static_cast<std::uint32_t> (r) * swatch + dy,
							rows[r].colors[c]);
		}
		std::printf ("  %s (%zu colours)\n", rows[r].label.c_str (),
				rows[r].colors.size ());
	}

	std::string encErr;
	const auto png =
			encodeRgbaPng (canvas.width, canvas.height, canvas.px, encErr);
	if (!png || !writeFile (out, *png))
	{
		std::printf ("could not write %s: %s\n", out.string ().c_str (),
				encErr.c_str ());
		return 1;
	}
	std::printf ("%s: %zu palette row(s) -> %s\n",
			ctPath.filename ().string ().c_str (), rows.size (),
			out.string ().c_str ());
	return 0;
}

int
cmdFon (const fs::path &dir, const fs::path &out)
{
	std::vector<std::string> problems;
	const Font font = loadFontDir (dir, problems);
	for (const std::string &p : problems)
		std::printf ("  %s\n", p.c_str ());
	if (font.glyphs.empty ())
	{
		std::printf ("no glyphs in %s\n", dir.string ().c_str ());
		return 1;
	}

	std::vector<PngImage> images;
	std::uint32_t maxW = 1, maxH = 1;
	for (const Glyph &g : font.glyphs)
	{
		std::string err;
		const auto img = decodePng (readFile (g.file), err);
		if (!img)
		{
			std::printf ("  %s: %s\n", g.file.filename ().string ().c_str (),
					err.c_str ());
			images.emplace_back ();
			continue;
		}
		maxW = std::max (maxW, img->width);
		maxH = std::max (maxH, img->height);
		images.push_back (*img);
	}

	Sheet sheet = makeSheet (images.size (), maxW, maxH);
	for (std::size_t i = 0; i < images.size (); ++i)
	{
		const std::uint32_t cx =
				static_cast<std::uint32_t> (i % sheet.cols) * sheet.cellW;
		const std::uint32_t cy =
				static_cast<std::uint32_t> (i / sheet.cols) * sheet.cellH;
		blit (sheet.canvas, images[i], cx + 4, cy + 4, nullptr);
		sheet.canvas.rect (cx, cy, sheet.cellW, sheet.cellH, Rgb{90, 90, 100});
	}

	std::string err;
	const auto png = encodeRgbaPng (sheet.canvas.width, sheet.canvas.height,
			sheet.canvas.px, err);
	if (!png || !writeFile (out, *png))
	{
		std::printf ("could not write %s: %s\n", out.string ().c_str (),
				err.c_str ());
		return 1;
	}
	std::printf ("%s: %zu glyphs (U+%04X..U+%04X) -> %s\n",
			dir.filename ().string ().c_str (), font.glyphs.size (),
			static_cast<unsigned> (font.glyphs.front ().codepoint),
			static_cast<unsigned> (font.glyphs.back ().codepoint),
			out.string ().c_str ());
	return 0;
}

int
usage ()
{
	std::printf (
			"uqm2-browse -- content browser (docs/content-formats.md)\n"
			"\n"
			"  uqm2-browse inventory <content>\n"
			"        resource counts, dangling keys, rmp problems\n"
			"\n"
			"  uqm2-browse ani <file.ani> <out.png> [colortable.ct]\n"
			"        contact sheet of every cel, hotspots marked. Give the\n"
			"        .ct and the cels are coloured through the colormap slot\n"
			"        their .ani names -- which is how the game draws them,\n"
			"        and the binding nothing else checks.\n"
			"\n"
			"  uqm2-browse ct <file.ct> <out.png>\n"
			"        palette swatches, one row per table, saying which of the\n"
			"        two shapes each entry turned out to be\n"
			"\n"
			"  uqm2-browse fon <dir.fon> <out.png>\n"
			"        every glyph in a font directory\n");
	return 2;
}

}  // namespace

int
main (int argc, char **argv)
{
	if (argc < 2)
		return usage ();

	const std::string cmd = argv[1];
	if (cmd == "inventory" && argc == 3)
		return cmdInventory (argv[2]);
	if (cmd == "ani" && (argc == 4 || argc == 5))
		return cmdAni (argv[2], argv[3], argc == 5 ? argv[4] : "");
	if (cmd == "ct" && argc == 4)
		return cmdCt (argv[2], argv[3]);
	if (cmd == "fon" && argc == 4)
		return cmdFon (argv[2], argv[3]);

	return usage ();
}
