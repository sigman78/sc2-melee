// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.
//
// Content library tests. Two halves, and both matter:
//
//   - unit cases over hand-built bytes, including the malformed ones, since
//     "rejects garbage without mis-reading it" is most of what this library
//     is for;
//   - a sweep over the whole real content tree, because the formats'
//     surprises are empirical (docs/content-formats.md) and a parser that
//     only ever sees three curated files will not meet them.
//
// No framework, matching tests/coroutine_test.c: non-zero exit means failure.

#include "engine/content/AniFile.hpp"
#include "engine/content/BinaryTable.hpp"
#include "engine/content/ColorTable.hpp"
#include "engine/content/FontDir.hpp"
#include "engine/content/PhraseFile.hpp"
#include "engine/content/PngImage.hpp"
#include "engine/content/ResourceMap.hpp"
#include "engine/content/Sprite.hpp"
#include "engine/core/Geometry.hpp"
#include "platform/File.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

using namespace uqm;
using namespace uqm::content;
namespace fs = std::filesystem;

namespace {

int failures = 0;

#define CHECK(cond, ...)                                                      \
	do                                                                        \
	{                                                                         \
		if (!(cond))                                                          \
		{                                                                     \
			std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                  \
			std::printf(__VA_ARGS__);                                         \
			std::printf("\n");                                                \
			++failures;                                                       \
		}                                                                     \
	} while (0)

// --------------------------------------------------------------------------
// Core types

void
testClosedRangeIsClosed()
{
	// The whole reason this type is not called Range: [128, 255] is 128
	// elements, and its half-open end would be 256, which does not fit in the
	// uint8_t the format uses.
	constexpr ClosedRangeU8 planets{128, 255};
	static_assert(planets.count() == 128);
	static_assert(planets.contains(128) && planets.contains(255));
	static_assert(!planets.contains(127));

	constexpr ClosedRangeU8 single{10, 10};
	static_assert(single.count() == 1, "a closed [10,10] is one element");

	constexpr ClosedRangeU8 inverted{5, 1};
	static_assert(!inverted.valid());
	static_assert(inverted.count() == 0);
}

void
testGeometry()
{
	static_assert(Extent2u{4, 4}.area() == 16);
	static_assert(Extent2u{0, 4}.empty());
	static_assert(Extent2u{4, 4}.contains(Vec2u{3, 3}));
	static_assert(!Extent2u{4, 4}.contains(Vec2u{4, 0}));
	// Signed coordinates outside the extent, which the browser's hotspot
	// crosshair produces routinely.
	static_assert(!Extent2i{4, 4}.contains(Vec2i{-1, 0}));
	static_assert(Vec2i{1, 2} + Vec2i{3, 4} == Vec2i{4, 6});
	static_assert(Vec2i{5, 5} - Vec2i{1, 2} == Vec2i{4, 3});
}

void
testBigEndianReads()
{
	constexpr std::array<std::byte, 4> bytes{
		std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}};
	CHECK(readU8(bytes, 0) == 0x12, "readU8");
	CHECK(readU16BE(bytes, 0) == 0x1234, "readU16BE");
	CHECK(readU32BE(bytes, 0) == 0x12345678u, "readU32BE");
	CHECK(readU16BE(bytes, 2) == 0x5678, "readU16BE at an offset");
	CHECK(fits(bytes, 0, 4) && !fits(bytes, 1, 4), "fits");
	// The overflow the obvious `at + len <= size` would miss.
	CHECK(!fits(bytes, 2, static_cast<std::size_t>(-1)),
			"fits must not overflow");
}

// --------------------------------------------------------------------------
// Unit cases

void
testBinaryTableRejectsGarbage()
{
	CHECK(!parseBinaryTable({}), "empty input should not parse");

	// A compressed prefix. loadres.c refuses these and so do we.
	constexpr std::array<std::byte, 12> compressed{std::byte{0}, std::byte{0},
		std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
		std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
	const auto lz = parseBinaryTable(compressed);
	CHECK(!lz, "an LZ length prefix should be refused");
	if (!lz)
		CHECK(lz.error().code == ContentErrorCode::Compressed,
				"error should be Compressed");

	// count claims one entry of 16 bytes but the file holds none.
	constexpr std::array<std::byte, 16> truncated{std::byte{0xFF},
		std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0},
		std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0}, std::byte{0},
		std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
		std::byte{16}};
	const auto over = parseBinaryTable(truncated);
	CHECK(!over, "an entry running past EOF should be refused");
	if (!over)
		CHECK(over.error().code == ContentErrorCode::EntryOverruns,
				"error should be EntryOverruns");
}

void
testColorTableShapesAreNotInterchangeable()
{
	// A one-slot Palettes entry: [10, 10] + 768 bytes.
	std::vector<std::byte> palettes(2 + kPaletteBytes, std::byte{0});
	palettes[0] = std::byte{10};
	palettes[1] = std::byte{10};
	palettes[2] = std::byte{0xAB};  // first channel of colour 0

	const auto asPalettes =
			parseColorTableEntry(palettes, ColorTableShape::Palettes);
	CHECK(asPalettes.has_value(), "770-byte entry should parse as Palettes");
	if (asPalettes)
	{
		CHECK(asPalettes->paletteCount() == 1, "expected 1 palette, got %zu",
				asPalettes->paletteCount());
		// Extra parens: a braced initialiser's comma would otherwise split
		// the macro's arguments.
		CHECK((asPalettes->range() == ClosedRangeU8{10, 10}), "slot range");
		CHECK(asPalettes->palette(0)[0].r == 0xAB,
				"palette should carry the file's own bytes");
	}

	// The same bytes read as a partial palette must fail, not silently take
	// the first three. This is the whole reason the shape is a parameter.
	CHECK(!parseColorTableEntry(palettes, ColorTableShape::PartialPalette),
			"Palettes bytes must not parse as PartialPalette");

	// And the converse: a planets-style entry, [128, 255] + 128*3.
	std::vector<std::byte> partial(2 + 128 * kRgbSize, std::byte{0});
	partial[0] = std::byte{128};
	partial[1] = std::byte{255};
	const auto asPartial =
			parseColorTableEntry(partial, ColorTableShape::PartialPalette);
	CHECK(asPartial.has_value(), "386-byte entry should parse as partial");
	if (asPartial)
		CHECK(asPartial->colorCount() == 128, "expected 128 colours, got %zu",
				asPartial->colorCount());
	CHECK(!parseColorTableEntry(partial, ColorTableShape::Palettes),
			"PartialPalette bytes must not parse as Palettes");

	// An inverted range is refused, as SetColorMap refuses it.
	std::vector<std::byte> inverted(2, std::byte{0});
	inverted[0] = std::byte{5};
	inverted[1] = std::byte{1};
	const auto bad = parseColorTableEntry(inverted, ColorTableShape::Palettes);
	CHECK(!bad, "start > end should be refused");
	if (!bad)
		CHECK(bad.error().code == ContentErrorCode::InvertedRange,
				"error should be InvertedRange");
}

void
testAniRejectsWhatTheCWouldAbsorb()
{
	std::vector<ContentError> problems;

	// A blank line is the case the C turns into a duplicated cel.
	const AniFile blank =
			parseAni("a.png -1 10 0 0\r\n\r\nb.png -1 10 1 2\r\n", &problems);
	CHECK(blank.cels.size() == 2, "expected 2 cels, got %zu",
			blank.cels.size());
	CHECK(problems.size() == 1,
			"expected 1 complaint about the blank line, got %zu",
			problems.size());

	problems.clear();
	const AniFile shortLine = parseAni("a.png -1 10\r\n", &problems);
	CHECK(shortLine.cels.empty(), "a 3-field line should not yield a cel");
	CHECK(problems.size() == 1, "expected 1 complaint, got %zu",
			problems.size());

	problems.clear();
	const std::string_view text = "supox-001.png -1 10 -81 -30\r\n";
	const AniFile ok = parseAni(text, &problems);
	CHECK(problems.empty(), "clean line should have no complaints");
	CHECK(ok.cels.size() == 1, "expected 1 cel");
	if (ok.cels.size() == 1)
	{
		CHECK(ok.cels[0].file == "supox-001.png", "filename");
		// The name is a view into the source, not a copy of it.
		CHECK(ok.cels[0].file.data() == text.data(),
				"filename should view the source text, not copy it");
		CHECK(ok.cels[0].transparency == Transparency::None,
				"-1 means no transparency");
		CHECK(ok.cels[0].colormapIndex == 10, "colormap slot");
		CHECK((ok.cels[0].hotspot == Vec2i{-81, -30}), "hotspots are signed");
	}

	// No diagnostics wanted: nothing is allocated for them.
	const AniFile quiet = parseAni("a.png -1 10\r\n");
	CHECK(quiet.cels.empty(), "parseAni must work without a problems sink");
}

void
testPhraseHashHandling()
{
	// "#()" must not open a phrase: strtok returns NULL on it, so the C skips
	// the line entirely. Getting this wrong shifts every ordinal after it.
	//
	// The shape that occurs in base/gamestrings.txt: a "#()" followed only by
	// blank lines. The trailing trim removes them, so the body view is exact.
	{
		std::vector<ContentError> p;
		const PhraseFile pf =
				parsePhrases("#(A)\nfirst\n#()\n\n#(B)\nsecond\n", &p);
		CHECK(pf.size() == 2, "#() should not open a phrase, got %zu",
				pf.size());
		CHECK(p.empty(), "a trailing #() is not a problem, got %zu", p.size());
		if (pf.size() == 2)
		{
			CHECK(pf[0].text == "first", "body should stop before the #(), got "
										 "'%.*s'",
					static_cast<int>(pf[0].text.size()), pf[0].text.data());
			CHECK(pf[1].name == "B", "second phrase should be B");
			CHECK(pf.byOrdinal(1) == &pf[0], "ordinal 1 is phrase 0");
			CHECK(pf.byOrdinal(0) == nullptr, "ordinal 0 means 'say nothing'");
			CHECK(pf.byName("B") == &pf[1], "name lookup");
		}
	}

	// The shape that does NOT occur, and that a contiguous view cannot
	// represent: real text after a dropped "#()". The C concatenates across
	// it; this must complain rather than hand back a body with the "#()"
	// still inside it.
	{
		std::vector<ContentError> p;
		const PhraseFile pf =
				parsePhrases("#(A)\nfirst\n#()\nstray\n#(B)\nx\n", &p);
		CHECK(pf.size() == 2, "still two phrases");
		CHECK(p.size() == 1,
				"text after a dropped #() must be reported, got %zu problems",
				p.size());
	}
}

// --------------------------------------------------------------------------
// The real tree

void
sweepContent(const fs::path &content)
{
	const auto rmpBytes = platform::readFile(content / "uqm.rmp");
	CHECK(rmpBytes.has_value(), "cannot read uqm.rmp");
	if (!rmpBytes)
		return;

	std::vector<ContentError> problems;
	const ResourceMap map =
			ResourceMap::parse(platform::asText(*rmpBytes), &problems);
	CHECK(problems.empty(), "uqm.rmp had %zu unparseable lines",
			problems.size());
	CHECK(map.size() == 963, "expected 963 resource keys, got %zu", map.size());

	const Resource *supox = map.find("comm.supox.dialogue");
	CHECK(supox != nullptr, "comm.supox.dialogue should be in the map");
	if (supox != nullptr)
	{
		CHECK(supox->type == "CONVERSATION", "type");
		CHECK(supox->path == "base/comm/supox/supox.txt", "path");
		CHECK(supox->isPath(), "a CONVERSATION value is a path");
	}
	const Resource *ship = map.find("ship.supox.code");
	CHECK(ship != nullptr && !ship->isPath(),
			"a SHIP value is an index, not a path");
	CHECK(map.find("no.such.key") == nullptr, "missing key returns nullptr");

	// One buffer for the whole sweep, reused (rule 1).
	std::vector<std::byte> buffer;

	// Every .ct must parse as a container, and every entry as one of the two
	// shapes -- the counts are pinned so a content change that introduces a
	// third shape fails here rather than in the renderer.
	std::size_t ctFiles = 0, shapeA = 0, shapeB = 0;
	for (const auto &e : fs::recursive_directory_iterator(content))
	{
		if (!e.is_regular_file() || e.path().extension() != ".ct")
			continue;
		++ctFiles;

		const auto bytes = platform::readFileInto(e.path(), buffer);
		CHECK(bytes.has_value(), "cannot read %s", e.path().string().c_str());
		if (!bytes)
			continue;

		const auto table = parseBinaryTable(*bytes);
		CHECK(table.has_value(), "%s did not parse as a binary table",
				e.path().string().c_str());
		if (!table)
			continue;

		for (const Bytes &entry : *table)
		{
			if (parseColorTableEntry(entry, ColorTableShape::Palettes))
				++shapeA;
			else if (parseColorTableEntry(
							 entry, ColorTableShape::PartialPalette))
				++shapeB;
			else
				CHECK(false, "%s: entry of %zu bytes is neither shape",
						e.path().string().c_str(), entry.size());
		}
	}
	CHECK(ctFiles == 75, "expected 75 .ct files, found %zu", ctFiles);
	CHECK(shapeA == 79, "expected 79 full-palette entries, found %zu", shapeA);
	CHECK(shapeB == 123, "expected 123 partial-palette entries, found %zu",
			shapeB);

	// Every .ani must parse with no complaints at all.
	std::size_t aniFiles = 0, cels = 0;
	for (const auto &e : fs::recursive_directory_iterator(content))
	{
		if (!e.is_regular_file() || e.path().extension() != ".ani")
			continue;
		++aniFiles;

		const auto bytes = platform::readFileInto(e.path(), buffer);
		if (!bytes)
		{
			CHECK(false, "cannot read %s", e.path().string().c_str());
			continue;
		}
		std::vector<ContentError> aniProblems;
		const AniFile ani = parseAni(platform::asText(*bytes), &aniProblems);
		CHECK(aniProblems.empty(), "%s: %zu problems",
				e.path().string().c_str(), aniProblems.size());
		cels += ani.cels.size();
	}
	CHECK(aniFiles == 584, "expected 584 .ani files, found %zu", aniFiles);
	CHECK(cels == 6984, "expected 6984 cels, found %zu", cels);

	// Phrase files, driven off the resource map rather than a glob: not every
	// .txt in the tree is a resource. base/cutscene/ending/pc_credits.txt
	// says in its own header that it is reference material "not used directly
	// in UQM", and asserting things about files the game never opens is how a
	// checker acquires false failures and then gets ignored.
	std::size_t txtFiles = 0, phrases = 0, convFiles = 0, convPhrases = 0;
	std::map<std::string, fs::path> byStem;
	for (const Resource &res : map)
	{
		if (res.type != "CONVERSATION" && res.type != "STRTAB")
			continue;
		const fs::path path = content / res.path;
		if (!fs::exists(path))
		{
			CHECK(false, "%.*s does not exist",
					static_cast<int>(res.path.size()), res.path.data());
			continue;
		}
		++txtFiles;

		// Its own buffer: the phrases below are views into it.
		const auto bytes = platform::readFile(path);
		if (!bytes)
		{
			CHECK(false, "cannot read %s", path.string().c_str());
			continue;
		}
		std::vector<ContentError> p;
		const PhraseFile pf = parsePhrases(platform::asText(*bytes), &p);
		CHECK(p.empty(), "%.*s: %zu problems",
				static_cast<int>(res.path.size()), res.path.data(), p.size());
		phrases += pf.size();
		if (res.type == "CONVERSATION")
		{
			++convFiles;
			convPhrases += pf.size();
		}
		byStem.emplace(path.stem().string(), path);
	}

	// Cross-check against tools/check-phrases.py, which reaches the same 27
	// files by a different route (each race's *_CONVERSATION_PHRASES macro
	// through its resinst.h) and counts with a different parser. Two
	// implementations agreeing on 3,451 is worth more than either alone.
	CHECK(convFiles == 27, "expected 27 CONVERSATION resources, found %zu",
			convFiles);
	CHECK(convPhrases == 3451,
			"expected 3451 conversation phrases (what check-phrases.py "
			"reports), found %zu",
			convPhrases);

	std::size_t tsFiles = 0, tsOk = 0;
	for (const auto &e : fs::recursive_directory_iterator(content))
	{
		if (!e.is_regular_file() || e.path().extension() != ".ts")
			continue;
		++tsFiles;

		// A .ts pairs with the .txt beside it when there is one, else the
		// base file of the same name.
		fs::path partner = e.path();
		partner.replace_extension(".txt");
		if (!fs::exists(partner))
		{
			const auto it = byStem.find(e.path().stem().string());
			if (it == byStem.end())
			{
				CHECK(false, "%s: no .txt to pair with",
						e.path().string().c_str());
				continue;
			}
			partner = it->second;
		}

		// Both buffers stay alive for the whole comparison: the phrases are
		// views into one and their timestamps into the other.
		const auto txtBytes = platform::readFile(partner);
		const auto tsBytes = platform::readFile(e.path());
		if (!txtBytes || !tsBytes)
		{
			CHECK(false, "cannot read %s", e.path().string().c_str());
			continue;
		}

		std::vector<ContentError> p;
		PhraseFile pf = parsePhrases(platform::asText(*txtBytes), &p);
		if (attachTimestamps(pf, platform::asText(*tsBytes), &p))
			++tsOk;
		CHECK(p.empty(), "%s vs %s: %zu problems",
				e.path().filename().string().c_str(),
				partner.filename().string().c_str(), p.size());
	}
	CHECK(tsFiles == 27, "expected 27 .ts files, found %zu", tsFiles);
	CHECK(tsOk == tsFiles,
			"%zu of %zu timestamp files would be discarded wholesale by the C",
			tsFiles - tsOk, tsFiles);

	// Fonts are directories.
	std::size_t fontDirs = 0, glyphs = 0;
	for (const auto &e : fs::recursive_directory_iterator(content))
	{
		if (!e.is_directory() || e.path().extension() != ".fon")
			continue;
		++fontDirs;

		std::vector<ContentError> p;
		const Font f = loadFontDir(e.path(), &p);
		CHECK(p.empty(), "%s: %zu problems", e.path().string().c_str(),
				p.size());
		glyphs += f.glyphs.size();
	}
	CHECK(fontDirs == 30, "expected 30 .fon directories, found %zu", fontDirs);
	CHECK(glyphs == 2599, "expected 2599 glyphs, found %zu", glyphs);

	// Every PNG must decode. The colour-type histogram is printed rather than
	// asserted: it is the map of what the content actually uses.
	std::size_t pngs = 0, indexed = 0, rgba = 0, subByte = 0, keyed = 0;
	std::map<unsigned, std::size_t> byColorType, byBitDepth;
	for (const auto &e : fs::recursive_directory_iterator(content))
	{
		if (!e.is_regular_file() || e.path().extension() != ".png")
			continue;
		++pngs;

		const auto bytes = platform::readFileInto(e.path(), buffer);
		if (!bytes)
		{
			CHECK(false, "cannot read %s", e.path().string().c_str());
			continue;
		}
		const auto img = decodePng(*bytes);
		CHECK(img.has_value(), "%s did not decode", e.path().string().c_str());
		if (!img)
			continue;

		++byColorType[img->sourceColorType()];
		++byBitDepth[img->sourceBitDepth()];
		if (img->format() == PixelFormat::Indexed8)
		{
			++indexed;
			if (img->sourceBitDepth() < 8)
				++subByte;
			if (img->transparentIndex() >= 0)
				++keyed;
			CHECK(img->pixels().size() == img->size().area(),
					"%s: indexed buffer is %zu for %ux%u",
					e.path().filename().string().c_str(), img->pixels().size(),
					img->size().w, img->size().h);
			// An index with no palette entry draws as nothing in particular.
			for (const std::uint8_t px : img->pixels())
			{
				if (px >= img->paletteSize())
				{
					CHECK(false, "%s: index %u is past a %zu-entry palette",
							e.path().filename().string().c_str(), px,
							img->paletteSize());
					break;
				}
			}
		}
		else
		{
			++rgba;
			CHECK(img->pixels().size() == img->size().area() * 4,
					"%s: rgba buffer is %zu for %ux%u",
					e.path().filename().string().c_str(), img->pixels().size(),
					img->size().w, img->size().h);
		}
	}
	CHECK(pngs > 0, "no PNGs found");
	std::printf("  png: %zu decoded, %zu stayed indexed (%zu sub-byte, "
				"%zu colour-keyed), %zu became rgba\n",
			pngs, indexed, subByte, keyed, rgba);
	std::printf("  png colour types:");
	for (const auto &[type, n] : byColorType)
		std::printf(" %u=%zu", type, n);
	std::printf("   bit depths:");
	for (const auto &[depth, n] : byBitDepth)
		std::printf(" %u=%zu", depth, n);
	std::printf("\n");

	// Round-trip the encoder the browser writes sheets with.
	{
		constexpr Extent2u size{4, 4};
		std::vector<std::uint8_t> rgbaPixels(
				static_cast<std::size_t>(size.area()) * 4);
		for (std::size_t i = 0; i < rgbaPixels.size(); ++i)
			rgbaPixels[i] = static_cast<std::uint8_t>(i * 7);

		const auto encoded = encodeRgbaPng(size, rgbaPixels);
		CHECK(encoded.has_value(), "encode failed");
		if (encoded)
		{
			const auto back = decodePng(*encoded);
			CHECK(back.has_value(), "re-decode failed");
			if (back)
			{
				CHECK(back->size() == size, "size survived");
				CHECK(std::equal(back->pixels().begin(), back->pixels().end(),
							  rgbaPixels.begin()),
						"pixels survived the round trip");
			}
		}
		// A mismatched buffer is a caller error the encoder must catch.
		CHECK((!encodeRgbaPng(Extent2u{5, 5}, rgbaPixels)),
				"wrong-sized buffer must be refused");
	}

	std::printf("swept %zu .ct (%zu+%zu entries), %zu .ani (%zu cels), "
				"%zu .txt (%zu phrases), %zu .ts, %zu .fon (%zu glyphs), "
				"%zu resource keys\n",
			ctFiles, shapeA, shapeB, aniFiles, cels, txtFiles, phrases,
			tsFiles, fontDirs, glyphs, map.size());
}

}  // namespace

void
testOpacityBitsFollowAlpha()
{
	// The input a collision mask is built from. Per-pixel collision is only
	// worth having if the mask follows the silhouette, so a fully transparent
	// pixel must be clear and *any* non-zero alpha must be set -- including
	// the partially transparent edge pixels, which the C treats as solid
	// because its masks come from a colour-key, not a gradient.
	constexpr std::uint32_t w = 3;
	constexpr std::uint32_t h = 2;
	const std::vector<std::uint8_t> rgba{
			// row 0: opaque, clear, barely-there
			255, 0, 0, 255, /**/ 0, 0, 0, 0, /**/ 9, 9, 9, 1,
			// row 1: clear, opaque, clear
			0, 0, 0, 0, /**/ 1, 2, 3, 255, /**/ 4, 5, 6, 0};

	const std::vector<std::uint8_t> bits = opacityBits(rgba, Extent2u{w, h});
	const std::vector<std::uint8_t> want{1, 0, 1, 0, 1, 0};
	CHECK(bits == want, "opacity should follow alpha exactly");
	CHECK(bits.size() == static_cast<std::size_t>(w) * h,
			"one byte per pixel, got %zu", bits.size());
}

int
main(int argc, char **argv)
{
	testOpacityBitsFollowAlpha();
	testClosedRangeIsClosed();
	testGeometry();
	testBigEndianReads();
	testBinaryTableRejectsGarbage();
	testColorTableShapesAreNotInterchangeable();
	testAniRejectsWhatTheCWouldAbsorb();
	testPhraseHashHandling();

	if (argc > 1)
	{
		const fs::path content = argv[1];
		if (fs::exists(content / "uqm.rmp"))
			sweepContent(content);
		else
		{
			std::printf("FAIL no uqm.rmp under %s\n", argv[1]);
			++failures;
		}
	}
	else
	{
		std::printf("(no content directory given; unit cases only)\n");
	}

	if (failures != 0)
		std::printf("%d check(s) failed\n", failures);
	return failures != 0 ? 1 : 0;
}
