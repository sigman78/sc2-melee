// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.
//
// uqm2-browse -- a content browser for the formats in
// docs/content-formats.md.
//
// The plan (docs/game-rewrite-plan.md, M0) asks for a sprite/font/colormap
// browser on the grounds that "every byte-level surprise in the project lives
// here and this is the cheapest place to find it". This is that, as a CLI
// that writes PNG contact sheets rather than a window: it needs no platform
// layer beyond files, it runs headless, and its output is something you can
// attach to a bug report.
//
// This is also where the prose lives. The content library reports a
// ContentError -- a code and three numbers -- and every English sentence
// about content is formatted here, at the point it is shown
// (docs/cpp-conventions.md rules 2 and 3).
//
// The sprite sheets are the interesting part. A UQM sprite is an indexed PNG
// whose *displayed* colours come from a colormap slot named in the .ani and
// supplied by a .ct -- supox.ani says slot 10, supox.ct declares 10..10 --
// and nothing in the tree checks that those two agree. Rendering a sheet
// through the colormap makes that binding visible, and a broken one obvious.

#include "engine/content/AniFile.hpp"
#include "engine/content/BinaryTable.hpp"
#include "engine/content/ColorTable.hpp"
#include "engine/content/FontDir.hpp"
#include "engine/content/PngImage.hpp"
#include "engine/content/ResourceMap.hpp"
#include "engine/content/Sprite.hpp"
#include "platform/File.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace uqm;
using namespace uqm::content;
namespace fs = std::filesystem;

namespace {

// --------------------------------------------------------------------------
// Reporting

std::string
describeError(const ContentError &e)
{
	std::string out(describe(e.code));
	if (e.at != 0)
		out += std::format(" at {}", e.at);
	if (e.expected != 0 || e.actual != 0)
		out += std::format(" (expected {}, got {})", e.expected, e.actual);
	return out;
}

void
report(std::string_view what, const std::vector<ContentError> &problems)
{
	for (const ContentError &e : problems)
		std::printf("  %.*s: %s\n", static_cast<int>(what.size()), what.data(),
				describeError(e).c_str());
}

std::string
fileError(platform::FileError e)
{
	return std::string(platform::describe(e));
}

// --------------------------------------------------------------------------
// Canvas

// A plain RGBA canvas. Deliberately not an engine type: the browser draws for
// human eyes, and none of this belongs anywhere near the renderer.
class Canvas
{
public:
	explicit Canvas(Extent2u size)
		: size_(size), px_(static_cast<std::size_t>(size.area()) * 4, 0)
	{
	}

	[[nodiscard]] Extent2u size() const noexcept { return size_; }
	[[nodiscard]] std::span<const std::uint8_t> pixels() const noexcept
	{
		return px_;
	}

	// Clips rather than asserts: the hotspot crosshair legitimately falls
	// outside its cell, and "the hotspot is off the cel" is a property worth
	// seeing rather than a bug to trap.
	void
	set(Vec2u at, Rgb c, std::uint8_t a = 255) noexcept
	{
		if (!size_.contains(at))
			return;
		const std::size_t i =
				(static_cast<std::size_t>(at.y) * size_.w + at.x) * 4;
		px_[i + 0] = c.r;
		px_[i + 1] = c.g;
		px_[i + 2] = c.b;
		px_[i + 3] = a;
	}

	void
	fill(Rgb c) noexcept
	{
		for (std::uint32_t y = 0; y < size_.h; ++y)
			for (std::uint32_t x = 0; x < size_.w; ++x)
				set({x, y}, c);
	}

	// Transparency has to be visible, not merely absent -- "this cel has a
	// hole in it" and "this cel failed to load" look identical on black.
	void
	checkerboard(std::uint32_t cell = 8) noexcept
	{
		for (std::uint32_t y = 0; y < size_.h; ++y)
			for (std::uint32_t x = 0; x < size_.w; ++x)
				set({x, y},
						((x / cell) + (y / cell)) % 2 == 0 ? Rgb{40, 40, 46}
														   : Rgb{58, 58, 66});
	}

	void
	rect(Vec2u at, Extent2u extent, Rgb c) noexcept
	{
		for (std::uint32_t x = 0; x < extent.w; ++x)
		{
			set({at.x + x, at.y}, c);
			set({at.x + x, at.y + extent.h - 1}, c);
		}
		for (std::uint32_t y = 0; y < extent.h; ++y)
		{
			set({at.x, at.y + y}, c);
			set({at.x + extent.w - 1, at.y + y}, c);
		}
	}

	void
	crosshair(Vec2i at, Rgb c, std::int32_t arm = 3) noexcept
	{
		for (std::int32_t d = -arm; d <= arm; ++d)
		{
			if (at.x + d >= 0 && at.y >= 0)
				set(Vec2i{at.x + d, at.y}.as<std::uint32_t>(), c);
			if (at.x >= 0 && at.y + d >= 0)
				set(Vec2i{at.x, at.y + d}.as<std::uint32_t>(), c);
		}
	}

private:
	Extent2u size_;
	std::vector<std::uint8_t> px_;
};

bool
writeCanvas(const Canvas &canvas, const fs::path &out)
{
	const auto png = encodeRgbaPng(canvas.size(), canvas.pixels());
	if (!png)
	{
		std::printf("could not encode %s: %s\n", out.string().c_str(),
				describeError(png.error()).c_str());
		return false;
	}
	if (const auto ok = platform::writeFile(out, *png); !ok)
	{
		std::printf("could not write %s: %s\n", out.string().c_str(),
				fileError(ok.error()).c_str());
		return false;
	}
	return true;
}

// --------------------------------------------------------------------------
// Colormaps

// Slot -> palette, built from a .ct read in the Palettes shape. One entry
// covers a *range* of slots, so one entry can populate many.
using SlotMap = std::map<int, Palette>;

SlotMap
loadColormaps(const fs::path &ct)
{
	SlotMap slots;

	const auto bytes = platform::readFile(ct);
	if (!bytes)
	{
		std::printf("  %s: %s\n", ct.filename().string().c_str(),
				fileError(bytes.error()).c_str());
		return slots;
	}

	const auto table = parseBinaryTable(*bytes);
	if (!table)
	{
		std::printf("  %s: %s\n", ct.filename().string().c_str(),
				describeError(table.error()).c_str());
		return slots;
	}

	for (std::size_t i = 0; i < table->size(); ++i)
	{
		const auto entry =
				parseColorTableEntry((*table)[i], ColorTableShape::Palettes);
		if (!entry)
		{
			// Very likely a partial-palette table (a planets/*.ct). Not an
			// error, just not something a sprite sheet can colour with.
			std::printf("  %s entry %zu is not a full-palette table: %s\n",
					ct.filename().string().c_str(), i,
					describeError(entry.error()).c_str());
			continue;
		}
		for (std::size_t p = 0; p < entry->paletteCount(); ++p)
			slots[entry->range().first + static_cast<int>(p)] =
					entry->palette(p);
	}
	return slots;
}

// --------------------------------------------------------------------------
// Drawing content

// Expansion lives in the content library now (engine/content/Sprite.hpp), so
// the browser and the game colour a cel the same way. That matters more than
// it sounds: a `.ct` colormap and a PNG's own PLTE disagree by construction,
// so a second copy of this would be a second place for the two to drift.
void
blit(Canvas &c, const PngImage &img, Vec2u origin, const Palette *palette)
{
	const Extent2u size = img.size();
	const std::vector<std::uint8_t> rgba = toRgba(img, palette);

	for (std::uint32_t y = 0; y < size.h; ++y)
	{
		for (std::uint32_t x = 0; x < size.w; ++x)
		{
			const std::size_t i =
					(static_cast<std::size_t>(y) * size.w + x) * 4;
			if (rgba[i + 3] == 0)
				continue;
			c.set(Vec2u{origin.x + x, origin.y + y},
					Rgb{rgba[i], rgba[i + 1], rgba[i + 2]}, rgba[i + 3]);
		}
	}
}

struct Sheet
{
	Canvas canvas;
	Extent2u cell;
	std::uint32_t cols = 0;
};

Sheet
makeSheet(std::size_t count, Extent2u largest)
{
	constexpr std::uint32_t kPad = 4;
	const Extent2u cell{largest.w + kPad * 2, largest.h + kPad * 2};
	const auto cols = static_cast<std::uint32_t>(std::max<std::size_t>(1,
			static_cast<std::size_t>(
					std::ceil(std::sqrt(static_cast<double>(count))))));
	const auto rows = static_cast<std::uint32_t>((count + cols - 1) / cols);

	Sheet s{Canvas(Extent2u{cols * cell.w, std::max(rows, 1u) * cell.h}), cell,
		cols};
	s.canvas.checkerboard();
	return s;
}

// Decodes every image named by `paths`, returning them alongside the largest
// extent so the sheet can be laid out in one pass. Failures become default
// PngImages, which draw as nothing and are reported.
std::pair<std::vector<PngImage>, Extent2u>
decodeAll(const std::vector<fs::path> &paths)
{
	std::vector<PngImage> images;
	images.reserve(paths.size());
	Extent2u largest{1, 1};

	// One buffer, reused across every file (rule 1).
	std::vector<std::byte> buffer;
	for (const fs::path &p : paths)
	{
		const auto bytes = platform::readFileInto(p, buffer);
		if (!bytes)
		{
			std::printf("  %s: %s\n", p.filename().string().c_str(),
					fileError(bytes.error()).c_str());
			images.emplace_back();
			continue;
		}
		auto img = decodePng(*bytes);
		if (!img)
		{
			std::printf("  %s: %s\n", p.filename().string().c_str(),
					describeError(img.error()).c_str());
			images.emplace_back();
			continue;
		}
		largest.w = std::max(largest.w, img->size().w);
		largest.h = std::max(largest.h, img->size().h);
		images.push_back(std::move(*img));
	}
	return {std::move(images), largest};
}

// --------------------------------------------------------------------------
// Commands

int
cmdInventory(const fs::path &content)
{
	const auto text = platform::readFile(content / "uqm.rmp");
	if (!text)
	{
		std::printf("cannot read uqm.rmp: %s\n",
				fileError(text.error()).c_str());
		return 1;
	}

	std::vector<ContentError> problems;
	const ResourceMap map =
			ResourceMap::parse(platform::asText(*text), &problems);

	std::map<std::string_view, std::size_t> byType;
	std::size_t missing = 0;
	for (const Resource &res : map)
	{
		++byType[res.type];

		// Not every value is a path: a SHIP value is an index into a table
		// compiled into the binary. Checking those for existence reports all
		// 28 of them as dangling, which is how the exception was found.
		if (!res.isPath())
			continue;

		if (!fs::exists(content / res.path))
		{
			std::printf("  dangling: %.*s -> %.*s\n",
					static_cast<int>(res.key.size()), res.key.data(),
					static_cast<int>(res.path.size()), res.path.data());
			++missing;
		}
	}

	std::printf("%zu resources in uqm.rmp\n", map.size());
	for (const auto &[type, n] : byType)
		std::printf("  %-14.*s %zu\n", static_cast<int>(type.size()),
				type.data(), n);
	std::printf("  %zu dangling\n", missing);

	std::size_t files = 0;
	for (const auto &e : fs::recursive_directory_iterator(content))
		if (e.is_regular_file())
			++files;
	std::printf("  %zu files in the tree\n", files);

	report("rmp", problems);
	return problems.empty() && missing == 0 ? 0 : 1;
}

int
cmdAni(const fs::path &aniPath, const fs::path &out, const fs::path &ctPath)
{
	const auto text = platform::readFile(aniPath);
	if (!text)
	{
		std::printf("cannot read %s: %s\n", aniPath.string().c_str(),
				fileError(text.error()).c_str());
		return 1;
	}

	std::vector<ContentError> problems;
	const AniFile ani = parseAni(platform::asText(*text), &problems);
	report(aniPath.filename().string(), problems);
	if (ani.cels.empty())
	{
		std::printf("no cels in %s\n", aniPath.string().c_str());
		return 1;
	}

	const SlotMap slots = ctPath.empty() ? SlotMap{} : loadColormaps(ctPath);

	std::vector<fs::path> paths;
	paths.reserve(ani.cels.size());
	for (const Cel &cel : ani.cels)
		paths.push_back(aniPath.parent_path() / cel.file);

	auto [images, largest] = decodeAll(paths);
	Sheet sheet = makeSheet(images.size(), largest);

	std::size_t missingSlots = 0;
	for (std::size_t i = 0; i < images.size(); ++i)
	{
		const Vec2u cellAt{
			static_cast<std::uint32_t>(i % sheet.cols) * sheet.cell.w,
			static_cast<std::uint32_t>(i / sheet.cols) * sheet.cell.h};
		const Vec2u origin{cellAt.x + 4, cellAt.y + 4};

		const Palette *pal = nullptr;
		if (!slots.empty() && images[i].format() == PixelFormat::Indexed8)
		{
			const auto it = slots.find(ani.cels[i].colormapIndex);
			if (it != slots.end())
				pal = &it->second;
			else
				++missingSlots;
		}

		blit(sheet.canvas, images[i], origin, pal);

		// The hotspot is the cel's origin, and it is routinely negative --
		// supox-001 is at (-81, -30).
		sheet.canvas.crosshair(
				origin.as<std::int32_t>() - ani.cels[i].hotspot,
				Rgb{255, 220, 0});
		sheet.canvas.rect(cellAt, sheet.cell, Rgb{90, 90, 100});
	}

	if (missingSlots != 0)
	{
		std::printf("  %zu cel(s) name a colormap slot the .ct does not "
					"supply\n",
				missingSlots);
	}

	if (!writeCanvas(sheet.canvas, out))
		return 1;

	std::printf("%s: %zu cels -> %s (%ux%u)%s\n",
			aniPath.filename().string().c_str(), ani.cels.size(),
			out.string().c_str(), sheet.canvas.size().w, sheet.canvas.size().h,
			slots.empty() ? " [PNG palettes]" : " [colormapped]");
	return 0;
}

int
cmdCt(const fs::path &ctPath, const fs::path &out)
{
	const auto bytes = platform::readFile(ctPath);
	if (!bytes)
	{
		std::printf("cannot read %s: %s\n", ctPath.string().c_str(),
				fileError(bytes.error()).c_str());
		return 1;
	}

	const auto table = parseBinaryTable(*bytes);
	if (!table)
	{
		std::printf("%s: %s\n", ctPath.string().c_str(),
				describeError(table.error()).c_str());
		return 1;
	}

	// Each entry is tried as both shapes and the sheet says which it was --
	// the ambiguity is the headline fact about this format, so the browser
	// shows it rather than hiding it behind a guess.
	struct Row
	{
		std::vector<Rgb> colors;
		std::string label;
	};
	std::vector<Row> rows;

	for (std::size_t i = 0; i < table->size(); ++i)
	{
		if (const auto a =
						parseColorTableEntry((*table)[i], ColorTableShape::Palettes))
		{
			for (std::size_t p = 0; p < a->paletteCount(); ++p)
			{
				const Palette pal = a->palette(p);
				rows.push_back(Row{{pal.begin(), pal.end()},
					std::format("entry {} slot {}", i,
							a->range().first + static_cast<int>(p))});
			}
		}
		else if (const auto b = parseColorTableEntry(
						 (*table)[i], ColorTableShape::PartialPalette))
		{
			Row row;
			row.colors.reserve(b->colorCount());
			for (std::size_t k = 0; k < b->colorCount(); ++k)
				row.colors.push_back(b->color(k));
			row.label = std::format("entry {} indices {}..{}", i,
					b->range().first, b->range().last);
			rows.push_back(std::move(row));
		}
		else
		{
			std::printf("  entry %zu is neither shape\n", i);
		}
	}

	if (rows.empty())
		return 1;

	constexpr std::uint32_t kSwatch = 8;
	std::uint32_t widest = 0;
	for (const Row &r : rows)
		widest = std::max(widest, static_cast<std::uint32_t>(r.colors.size()));

	Canvas canvas(Extent2u{
		widest * kSwatch, static_cast<std::uint32_t>(rows.size()) * kSwatch});
	canvas.fill(Rgb{20, 20, 24});
	for (std::size_t r = 0; r < rows.size(); ++r)
	{
		for (std::size_t c = 0; c < rows[r].colors.size(); ++c)
			for (std::uint32_t dy = 0; dy < kSwatch; ++dy)
				for (std::uint32_t dx = 0; dx < kSwatch; ++dx)
					canvas.set({static_cast<std::uint32_t>(c) * kSwatch + dx,
									   static_cast<std::uint32_t>(r) * kSwatch
											   + dy},
							rows[r].colors[c]);
		std::printf("  %s (%zu colours)\n", rows[r].label.c_str(),
				rows[r].colors.size());
	}

	if (!writeCanvas(canvas, out))
		return 1;

	std::printf("%s: %zu palette row(s) -> %s\n",
			ctPath.filename().string().c_str(), rows.size(),
			out.string().c_str());
	return 0;
}

int
cmdFon(const fs::path &dir, const fs::path &out)
{
	std::vector<ContentError> problems;
	const Font font = loadFontDir(dir, &problems);
	report(dir.filename().string(), problems);
	if (font.glyphs.empty())
	{
		std::printf("no glyphs in %s\n", dir.string().c_str());
		return 1;
	}

	std::vector<fs::path> paths;
	paths.reserve(font.glyphs.size());
	for (const Glyph &g : font.glyphs)
		paths.push_back(g.file);

	auto [images, largest] = decodeAll(paths);
	Sheet sheet = makeSheet(images.size(), largest);
	for (std::size_t i = 0; i < images.size(); ++i)
	{
		const Vec2u cellAt{
			static_cast<std::uint32_t>(i % sheet.cols) * sheet.cell.w,
			static_cast<std::uint32_t>(i / sheet.cols) * sheet.cell.h};
		blit(sheet.canvas, images[i], {cellAt.x + 4, cellAt.y + 4}, nullptr);
		sheet.canvas.rect(cellAt, sheet.cell, Rgb{90, 90, 100});
	}

	if (!writeCanvas(sheet.canvas, out))
		return 1;

	std::printf("%s: %zu glyphs (U+%04X..U+%04X) -> %s\n",
			dir.filename().string().c_str(), font.glyphs.size(),
			static_cast<unsigned>(font.glyphs.front().codepoint),
			static_cast<unsigned>(font.glyphs.back().codepoint),
			out.string().c_str());
	return 0;
}

int
usage()
{
	std::printf(
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
main(int argc, char **argv)
{
	if (argc < 2)
		return usage();

	const std::string_view cmd = argv[1];
	if (cmd == "inventory" && argc == 3)
		return cmdInventory(argv[2]);
	if (cmd == "ani" && (argc == 4 || argc == 5))
		return cmdAni(argv[2], argv[3], argc == 5 ? argv[4] : "");
	if (cmd == "ct" && argc == 4)
		return cmdCt(argv[2], argv[3]);
	if (cmd == "fon" && argc == 4)
		return cmdFon(argv[2], argv[3]);

	return usage();
}
