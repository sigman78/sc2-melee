// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.
//
// Ships, projectiles, asteroids and the planet all draw from content. Anything
// without art yet -- blasts, mainly -- falls back to a coloured rectangle,
// which is deliberately ugly so it reads as missing rather than as a choice.
//
// Everything is positioned by its cel's *hotspot*, not its centre. Each facing
// has its own: the Cruiser's sixteen are at (7,19), (12,19), (16,15) and so
// on, because the ship is not symmetric and each rendering sits differently in
// its box. Centring instead makes the hull wander as it turns, and puts the
// sprite off the collision mask, which is anchored to that same hotspot.

#include "app/melee/Draw.hpp"
#include "app/melee/Game.hpp"

#include "engine/core/Geometry.hpp"
#include "game/SpriteSet.hpp"
#include "platform/Platform.hpp"
#include "sim/Element.hpp"
#include "sim/Ship.hpp"
#include "sim/World.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace uqm::melee {

namespace {

struct Colour
{
	std::uint8_t r, g, b;
};

// A colour packed as 0xRRGGBB, the form the C's own tables are quoted in.
constexpr Colour
rgb(std::uint32_t c) noexcept
{
	return Colour{static_cast<std::uint8_t>((c >> 16) & 0xFF),
			static_cast<std::uint8_t>((c >> 8) & 0xFF),
			static_cast<std::uint8_t>(c & 0xFF)};
}

[[nodiscard]] Colour
colourFor(const sim::Element &e) noexcept
{
	switch (e.kind)
	{
		case sim::ElementKind::Ship:
			return e.playerNr == 0 ? Colour{0x40, 0xC0, 0xFF}
								   : Colour{0xFF, 0x60, 0x40};
		case sim::ElementKind::Weapon:
			return Colour{0xFF, 0xE0, 0x60};
		case sim::ElementKind::Asteroid:
			return Colour{0x90, 0x88, 0x78};
		case sim::ElementKind::Planet:
			return Colour{0x60, 0x90, 0x50};
		case sim::ElementKind::Blast:
			return Colour{0xFF, 0xFF, 0xC0};
		default:
			return Colour{0xC0, 0xC0, 0xC0};
	}
}

// A 3x5 digit font, one bit per pixel, packed a row per nibble.
//
// Deliberately not the game's own font. That path exists -- FontDir parses
// .fon and the browser renders them -- but wiring it in means glyph atlases,
// kerning and a text layer, and this is a readout of two numbers per player.
// The real status panel is M2 work and will use the real fonts; this is
// scaffolding that says what the simulation thinks is true.
constexpr std::array<std::uint16_t, 10> kDigits{{
		0b111'101'101'101'111,  // 0
		0b010'110'010'010'111,  // 1
		0b111'001'111'100'111,  // 2
		0b111'001'111'001'111,  // 3
		0b101'101'111'001'001,  // 4
		0b111'100'111'001'111,  // 5
		0b111'100'111'101'111,  // 6
		0b111'001'001'001'001,  // 7
		0b111'101'111'101'111,  // 8
		0b111'101'111'001'111,  // 9
}};

void
drawDigit(platform::Platform &w, int digit, Vec2i at, int scale, Colour c)
{
	if (digit < 0 || digit > 9)
		return;
	const std::uint16_t bits = kDigits[static_cast<std::size_t>(digit)];
	for (int row = 0; row < 5; ++row)
	{
		for (int col = 0; col < 3; ++col)
		{
			// Most significant bit is the top-left pixel.
			const int bit = 14 - (row * 3 + col);
			if (((bits >> bit) & 1) == 0)
				continue;
			w.fillRect(Vec2i{at.x + col * scale, at.y + row * scale},
					Extent2u{static_cast<std::uint32_t>(scale),
						static_cast<std::uint32_t>(scale)},
					c.r, c.g, c.b);
		}
	}
}

// Right-aligned, so a number shrinking from 18 to 9 does not shift about.
void
drawNumber(platform::Platform &w, std::int32_t value, Vec2i rightTop,
		int scale, Colour c)
{
	if (value < 0)
		value = 0;
	int x = rightTop.x - 3 * scale;
	do
	{
		drawDigit(w, static_cast<int>(value % 10), Vec2i{x, rightTop.y}, scale,
				c);
		value /= 10;
		x -= 4 * scale;
	} while (value != 0);
}

// The starfield.
//
// Three planes, 30 big, 60 medium and 90 small (galaxy.c:37-44). The C stores
// each plane in a coordinate space 2^plane larger than the arena and then
// shifts all three by the *same* delta each frame (galaxy.c:405-407), so a
// plane in a larger space slides proportionally less across the screen. That
// is where the parallax comes from, and it is the only thing about the C's
// implementation worth keeping: the rest of galaxy.c is an incremental
// scroll over a y-sorted array, wrapping stars one at a time as they fall off
// an edge, which exists to avoid recomputing 180 positions on a 386 and buys
// nothing here.
//
// Stated directly instead: plane p scrolls at 1/2^p of the camera. Plane 0 is
// nearest -- biggest, brightest, fastest -- and plane 2 is the far haze.
inline constexpr int kStarPlanes = 3;
inline constexpr std::array<int, kStarPlanes> kStarsPerPlane{{30, 60, 90}};

// Only a fallback, for when the art is missing. The cels carry their own
// colour and their own brightness, and they are already graded by plane --
// which is the whole reason they must be drawn as *frames* and not as
// silhouettes.
//
// Drawing the silhouette was the defect: a silhouette is a flat fill of every
// non-transparent pixel, and these glyphs are mostly near-black. The 11x11
// cel is a bright core with almost-invisible diffraction arms, and the 5x5 is
// a single bright pixel inside a faint halo. Filling them turned three subtle
// stars into solid blobs and made the sky unreadable.
inline constexpr std::array<Colour, kStarPlanes> kStarColours{{
		rgb(0x949CFC),
		rgb(0x808CFC),
		rgb(0xA4ACFC),
}};

// Which cel each plane draws: 11x11 near, 5x5 mid, a single pixel far.
// star_frame_ofs (galaxy.c:316) picks the largest group for the nearest plane
// and the smallest for the farthest, which is the same ordering.
// Everything one size down from where it started: the 11x11 cel is simply
// too large for a background at this resolution, so the nearest plane takes
// what the middle one had, and the middle takes a single pixel.
inline constexpr std::array<std::size_t, kStarPlanes> kStarCels{{0, 2, 2}};

// The Ilwrath cloak ramp, converted from the C's 5-bit RGB
// (ilwrath.c:255-275). Index 0 is unused -- level 0 means "draw the real
// sprite" -- and the last step is invisible, so it is not listed.
// cycle_ion_trail's colour table (tactrans.c:757-770), 5-bit RGB widened to
// 8. Yellow-white at the muzzle through red to almost-black, one step a frame.
constexpr std::array<Colour, 12> kIonRamp{{
		rgb(0xFFAD00),
		rgb(0xFF8C00),
		rgb(0xFF7300),
		rgb(0xFF5200),
		rgb(0xFF3900),
		rgb(0xFF1800),
		rgb(0xFF0000),
		rgb(0xDE0000),
		rgb(0xBD0000),
		rgb(0x9C0000),
		rgb(0x7B0000),
		rgb(0x5A0000),
}};

constexpr std::array<Colour, 6> kCloakRamp{{
		rgb(0xFFFFFF),  // unused; level 0 draws the sprite
		rgb(0xFFFFFF),  // 1F,1F,1F
		rgb(0x52FFFF),  // 0A,1F,1F
		rgb(0x00A5A5),  // 00,14,14
		rgb(0x5252FF),  // 0A,0A,1F
		rgb(0x0000A5),  // 00,00,14
}};

// Which sprite set an element draws from. Ownership decides the weapon art,
// because a missile belongs to whoever fired it.
const game::SpriteSet *
spritesFor(const Game &g, const sim::Element &e) noexcept
{
	const game::SpriteSet *set = nullptr;
	switch (e.kind)
	{
		case sim::ElementKind::Debris:
			return e.playerNr < 0 ? g.boom : g.blast;
		case sim::ElementKind::Ship:
		case sim::ElementKind::ShipShadow:
			// A warp-in shadow is the ship's own image, so it borrows the art.
			set = e.playerNr == 0 ? g.cruiser : g.avenger;
			break;
		case sim::ElementKind::Weapon:
			set = e.playerNr == 0 ? g.nuke : g.flame;
			break;
		case sim::ElementKind::Asteroid:
			set = g.rock;
			break;
		case sim::ElementKind::Planet:
			set = g.world;
			break;
		case sim::ElementKind::Laser:
			return nullptr;  // drawn as a line, not a sprite
		case sim::ElementKind::Blast:
			// Weapon blasts and asteroid debris share a kind but not art. The
			// rubble an asteroid leaves is unowned; a blast belongs to the
			// shot that made it.
			set = e.playerNr < 0 ? g.boom : g.blast;
			break;
		default:
			return nullptr;
	}
	return set != nullptr && set->valid() ? set : nullptr;
}

// The background, before anything in the arena.
//
// Stars do not zoom and they do not scale. They are meant to be at infinity,
// so putting them through camera.scale() was wrong twice over: it swelled
// them as the arena zoomed, and -- because the zoom drifts continuously as the
// ships close and separate -- it fed a moving divisor into every star's
// position, so the whole field shimmered under any pan or zoom. That was the
// jitter. The fix is not better rounding; it is that the zoom has no business
// in this calculation at all.
//
// What is left is a plain pan. Each plane is a screen-sized torus, and the
// camera's position enters it divided by 2^plane, so plane 0 slides a pixel
// for every display pixel the camera travels, plane 1 half that, plane 2 a
// quarter. Integer arithmetic throughout, one shift, nothing to round.
void
drawStars(Game &g)
{
	const Vec2i centre = g.camera.centre();

	const auto wrapTo = [](std::int32_t v, std::int32_t n) {
		v %= n;
		return v < 0 ? v + n : v;
	};

	std::size_t first = 0;
	for (int plane = 0; plane < kStarPlanes; ++plane)
	{
		const auto p = static_cast<std::size_t>(plane);
		const std::size_t count = static_cast<std::size_t>(kStarsPerPlane[p]);
		const Colour c = kStarColours[p];

		// World to display is a shift of two; the plane's own slowdown is
		// another `plane` on top of it.
		const std::int32_t ox = centre.x >> (sim::kOneShift + plane);
		const std::int32_t oy = centre.y >> (sim::kOneShift + plane);

		const game::SpriteSet *art = g.starArt;
		const std::size_t cel = kStarCels[p];
		const bool haveArt = art != nullptr && cel < art->frames.size()
				&& cel < art->masks.size();

		Extent2u size{1, 1};
		Vec2i hs{0, 0};
		if (haveArt)
		{
			size = art->masks[cel].size();
			hs = art->masks[cel].hotspot();
		}

		for (std::size_t i = first; i < first + count; ++i)
		{
			const std::int32_t sx = wrapTo(g.stars[i].x - ox, kStarFieldWidth);
			const std::int32_t sy = wrapTo(g.stars[i].y - oy, kStarFieldHeight);

			// Drawn up to four times so a star straddling the seam comes back
			// in on the other side instead of popping out of existence.
			for (int wx = 0; wx < 2; ++wx)
			{
				for (int wy = 0; wy < 2; ++wy)
				{
					const std::int32_t x = sx - wx * kStarFieldWidth - hs.x;
					const std::int32_t y = sy - wy * kStarFieldHeight - hs.y;
					if (x + static_cast<std::int32_t>(size.w) <= 0
							|| y + static_cast<std::int32_t>(size.h) <= 0
							|| x >= sim::kSpaceWidth || y >= sim::kSpaceHeight)
						continue;

					if (haveArt)
						g.window.draw(art->frames[cel], Vec2i{x, y}, size);
					else
						g.window.fillRect(
								Vec2i{x, y}, size, c.r, c.g, c.b);
				}
			}
		}

		first += count;
	}
}

void drawOverlay(Game &g);
void drawHud(Game &g);

}  // namespace

void
draw(Game &g)
{
	g.window.clear(0x08, 0x08, 0x18);
	drawStars(g);

	// Every position goes through the camera, which goes through wrapDelta:
	// the arena is a torus eight screens across, so an element just over the
	// seam is a few pixels away, not eight screens away. Getting that wrong
	// makes things jump the width of the display as they cross.
	for (sim::EntityId id = g.battle.elements().front(); id.valid();
			id = g.battle.elements().next(id))
	{
		auto e = g.battle.get(id);
		if (e == nullptr)
			continue;

		// Exhaust: a single point stepping through the colour ramp as it
		// ages. lifeSpan counts down, so the ramp index counts up.
		if (e->kind == sim::ElementKind::IonTrail)
		{
			const std::size_t step = static_cast<std::size_t>(
					sim::kIonTrailLife - e->lifeSpan);
			if (step >= kIonRamp.size())
				continue;
			const Colour c = kIonRamp[step];
			const Vec2i at = g.camera.toScreen(e->current);
			g.window.fillRect(at, Extent2u{1, 1}, c.r, c.g, c.b);
			continue;
		}

		// A spark of a dying ship: the boom animation, stepped by its own age
		// rather than by a facing, since it has none.
		if (e->kind == sim::ElementKind::Debris)
		{
			const game::SpriteSet *set = spritesFor(g, *e);
			const Vec2i at = g.camera.toScreen(e->current);
			if (set == nullptr || set->frames.empty())
			{
				g.window.fillRect(at, Extent2u{2, 2}, 0xFF, 0xC0, 0x40);
				continue;
			}
			const std::size_t frames = set->frames.size();
			const std::int32_t age = sim::kDebrisLife - e->lifeSpan;
			const std::size_t i = std::min(frames - 1,
					static_cast<std::size_t>(std::max(0, age))
							* frames / static_cast<std::size_t>(
									sim::kDebrisLife));
			const Extent2u sz = set->masks[i].size();
			const Vec2i hs = set->masks[i].hotspot();
			const std::int32_t dw =
					std::max(1, g.camera.scale(sim::displayToWorld(
											 static_cast<std::int32_t>(sz.w))));
			const std::int32_t dh =
					std::max(1, g.camera.scale(sim::displayToWorld(
											 static_cast<std::int32_t>(sz.h))));
			g.window.draw(set->frames[i],
					Vec2i{at.x - static_cast<std::int32_t>(hs.x) * dw
									/ std::max(1, static_cast<std::int32_t>(sz.w)),
						at.y - static_cast<std::int32_t>(hs.y) * dh
									/ std::max(1, static_cast<std::int32_t>(sz.h))},
					Extent2u{static_cast<std::uint32_t>(dw),
						static_cast<std::uint32_t>(dh)});
			continue;
		}

		// A beam is a line between two points, not a sprite at one. Drawn
		// before the width/height work below, none of which applies.
		if (e->kind == sim::ElementKind::Laser)
		{
			g.window.drawLine(g.camera.toScreen(e->current),
					g.camera.toScreen(e->next), 0xFF, 0xFF, 0xFF);
			continue;
		}

		const Vec2i at = g.camera.toScreen(e->current);

		// The sprite shrinks with the zoom along with everything else -- one
		// zoom, one scale, no separate LOD path.
		const Extent2u mask =
				e->mask != nullptr ? e->mask->size() : Extent2u{8, 8};
		const std::int32_t w = std::max(
				1, g.camera.scale(sim::displayToWorld(
						   static_cast<std::int32_t>(mask.w))));
		const std::int32_t h = std::max(
				1, g.camera.scale(sim::displayToWorld(
						   static_cast<std::int32_t>(mask.h))));

		if (at.x + w < 0 || at.y + h < 0 || at.x - w > sim::kSpaceWidth
				|| at.y - h > sim::kSpaceHeight)
			continue;

		const Extent2u dest{static_cast<std::uint32_t>(w),
				static_cast<std::uint32_t>(h)};

		// A ship that has not arrived yet is not drawn -- only the shadows it
		// sheds are (tactrans.c:863). And a dead one is drawn as its own
		// explosion, growing over the frames it burns for.
		if (e->kind == sim::ElementKind::Ship)
		{
			if (any(e->flags & sim::ElementFlags::NonSolid)
					&& e->ship.crew > 0)
				continue;  // still warping in

			// A dying ship keeps its own hull for the first fifteen frames
			// and only then stops being drawn (tactrans.c:569-571). The
			// explosion is not this element at all -- it is the swarm of
			// sparks explosionPreProcess throws off around it, each its own
			// Debris. Drawing one boom animation on the wreck instead, which
			// is what this did, replaced a ship coming apart with a puff.
			if (e->ship.crew == 0
					&& sim::kExplosionLife - e->lifeSpan >= sim::kHullVanishAge)
				continue;
		}

		if (const game::SpriteSet *set = spritesFor(g, *e); set != nullptr)
		{
			// A weapon draws the cel the simulation says it is -- colorCycle,
			// which is the facing for a directional missile and the animation
			// frame for the flame. Drawing by facing put the eight-frame
			// fireball on whichever cel the launch facing selected, frozen.
			const std::size_t i = e->kind == sim::ElementKind::Weapon
					? static_cast<std::size_t>(e->colorCycle)
							% set->frames.size()
					: static_cast<std::size_t>(e->facing.raw())
							% set->frames.size();

			// Draw from the cel's *hotspot*, not its centre.
			//
			// The hotspot is where the game considers the object to be, and
			// every cel has its own -- the Cruiser's sixteen facings are at
			// (7,19), (12,19), (16,15) and so on, because the ship is not
			// symmetric and each rendering sits differently in its box.
			// Centring instead makes the hull shift around as it turns, and
			// puts the sprite off the collision mask that is anchored to the
			// same hotspot.
			const Vec2i hs = set->masks[i].hotspot();
			const Extent2u src = set->masks[i].size();
			const std::int32_t ox = src.w != 0
					? static_cast<std::int32_t>(hs.x) * w
							/ static_cast<std::int32_t>(src.w)
					: w / 2;
			const std::int32_t oy = src.h != 0
					? static_cast<std::int32_t>(hs.y) * h
							/ static_cast<std::int32_t>(src.h)
					: h / 2;

			// A cloaking ship is a flat silhouette stepping through a fixed
			// colour ramp, not a faded sprite: white, cyan-white, dark cyan,
			// blue, dark blue, gone (ilwrath.c:250-285). Uncloaking runs the
			// same ramp backwards, and firing reverses it -- so an Avenger
			// that shoots while hidden lights itself up.
			// A warp-in shadow: the hull as a flat fill, stepping through the
			// exhaust ramp as it ages. Same STAMPFILL the cloak uses -- the C
			// uses that primitive for both, and for the same reason: what you
			// want is the ship's outline in one colour, not the ship.
			if (e->kind == sim::ElementKind::ShipShadow)
			{
				const std::size_t step = static_cast<std::size_t>(
						sim::kIonTrailLife - e->lifeSpan);
				if (step >= kIonRamp.size() || i >= set->silhouettes.size())
					continue;
				const Colour c = kIonRamp[step];
				g.window.drawTinted(set->silhouettes[i],
						Vec2i{at.x - ox, at.y - oy}, dest, c.r, c.g, c.b);
				continue;
			}

			const std::int32_t cloak = e->ship.cloakLevel;
			if (cloak > 0 && i < set->silhouettes.size())
			{
				// Levels 1..5 are the five fill colours; kCloakFullLevel is
				// BLACK. The C fills with BLACK_COLOR and that reads as gone
				// against its own black space -- but this renderer clears to
				// a dark blue, so a black fill leaves a ship-shaped hole,
				// which is worse than no cloak at all. Drawing nothing is
				// what the C means. (Hiding one level early, which is what
				// this did first, cut the ramp to four visible colours.)
				if (cloak >= sim::kCloakFullLevel)
					continue;
				const Colour c = kCloakRamp[static_cast<std::size_t>(cloak)];
				g.window.drawTinted(set->silhouettes[i],
						Vec2i{at.x - ox, at.y - oy}, dest, c.r, c.g, c.b);
				continue;
			}

			g.window.draw(set->frames[i], Vec2i{at.x - ox, at.y - oy}, dest);
			continue;
		}

		const Colour c = colourFor(*e);
		g.window.fillRect(Vec2i{at.x - w / 2, at.y - h / 2}, dest, c.r, c.g,
				c.b);
	}

	drawHud(g);

	if (g.debugOverlay)
		drawOverlay(g);

	g.window.present();
}

namespace {

// Crew and energy for both players, in the corners they own: player 0 top
// left, player 1 top right. Crew above energy, coloured to match the ship so
// there is nothing to read to know whose is whose.
//
// Drawn from the element, so a destroyed ship shows nothing rather than a
// stale number -- which is also how you tell "dead" from "at zero crew".
void
drawHud(Game &g)
{
	constexpr int kScale = 1;
	constexpr std::int32_t kMargin = 4;
	constexpr std::int32_t kLine = 7;

	for (std::size_t p = 0; p < g.ships.size(); ++p)
	{
		const auto e = g.battle.get(g.ships[p]);
		if (e == nullptr)
			continue;

		const Colour crewColour = colourFor(*e);
		constexpr Colour energyColour{0x60, 0xFF, 0xC0};

		// Player 0 hugs the left edge, player 1 the right. Both are drawn
		// right-aligned; only the anchor differs.
		const std::int32_t right = p == 0
				? kMargin + 3 * 4 * kScale
				: sim::kSpaceWidth - kMargin;

		drawNumber(g.window, e->ship.crew, Vec2i{right, kMargin}, kScale,
				crewColour);
		drawNumber(g.window, e->ship.energy, Vec2i{right, kMargin + kLine},
				kScale, energyColour);
	}
}

// The collision overlay: what touched, where, and what the response did.
//
// Reads Battle::collisions() rather than anything of its own, so what is drawn
// is exactly what the simulation resolved -- an overlay that recomputed the
// contact point could agree with itself while disagreeing with the physics,
// which would be worse than no overlay.
void
drawOverlay(Game &g)
{
	// Mask bounds, so it is visible when a silhouette is not what you expect.
	for (sim::EntityId id = g.battle.elements().front(); id.valid();
			id = g.battle.elements().next(id))
	{
		const auto e = g.battle.get(id);
		if (e == nullptr || e->mask == nullptr)
			continue;

		const Vec2i at = g.camera.toScreen(e->current);
		const Extent2u m = e->mask->size();
		const std::int32_t w = g.camera.scale(
				sim::displayToWorld(static_cast<std::int32_t>(m.w)));
		const std::int32_t h = g.camera.scale(
				sim::displayToWorld(static_cast<std::int32_t>(m.h)));
		const Vec2i hs = e->mask->hotspot();
		const std::int32_t ox = m.w != 0
				? static_cast<std::int32_t>(hs.x) * w
						/ static_cast<std::int32_t>(m.w)
				: w / 2;
		const std::int32_t oy = m.h != 0
				? static_cast<std::int32_t>(hs.y) * h
						/ static_cast<std::int32_t>(m.h)
				: h / 2;

		const Vec2i tl{at.x - ox, at.y - oy};
		const Vec2i br{tl.x + w, tl.y + h};
		g.window.drawLine(tl, Vec2i{br.x, tl.y}, 0x30, 0x60, 0x30);
		g.window.drawLine(Vec2i{br.x, tl.y}, br, 0x30, 0x60, 0x30);
		g.window.drawLine(br, Vec2i{tl.x, br.y}, 0x30, 0x60, 0x30);
		g.window.drawLine(Vec2i{tl.x, br.y}, tl, 0x30, 0x60, 0x30);

		// The hotspot itself, which is where the game thinks the thing *is*.
		g.window.drawLine(Vec2i{at.x - 2, at.y}, Vec2i{at.x + 2, at.y}, 0x40,
				0xFF, 0x40);
		g.window.drawLine(Vec2i{at.x, at.y - 2}, Vec2i{at.x, at.y + 2}, 0x40,
				0xFF, 0x40);
	}

	// Contact points and response vectors. Velocities are scaled up because a
	// frame of travel is a handful of world units and an unscaled arrow would
	// be a dot.
	constexpr std::int32_t kVectorGain = 8;
	for (const Game::Mark &mark : g.marks)
	{
		const std::int64_t age =
				static_cast<std::int64_t>(g.battle.frame()) - mark.frame;
		if (age > kMarkLife)
			continue;

		const Vec2i at = g.camera.toScreen(mark.event.at);
		g.window.drawLine(Vec2i{at.x - 4, at.y - 4}, Vec2i{at.x + 4, at.y + 4},
				0xFF, 0x30, 0x30);
		g.window.drawLine(Vec2i{at.x - 4, at.y + 4}, Vec2i{at.x + 4, at.y - 4},
				0xFF, 0x30, 0x30);

		// Before in dim, after in bright: the difference *is* the response.
		const auto arrow = [&](Vec2i v, std::uint8_t r, std::uint8_t gg,
								   std::uint8_t b) {
			g.window.drawLine(at,
					Vec2i{at.x + g.camera.scale(v.x * kVectorGain),
						at.y + g.camera.scale(v.y * kVectorGain)},
					r, gg, b);
		};
		arrow(mark.event.beforeA, 0x60, 0x60, 0x90);
		arrow(mark.event.beforeB, 0x60, 0x60, 0x90);
		arrow(mark.event.afterA, 0x60, 0xC0, 0xFF);
		arrow(mark.event.afterB, 0xFF, 0xC0, 0x60);
	}
}

}  // namespace

}  // namespace uqm::melee
