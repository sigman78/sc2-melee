// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.
//
// Ships, projectiles, asteroids and the planet draw from content; anything
// without art yet falls back to a coloured rectangle, deliberately ugly so
// it reads as missing rather than as a choice.
//
// Positioning is by each cel's hotspot, not its centre -- facings are not
// symmetric and each sits differently in its box, so centring would make
// the hull wander and drift off the collision mask, which shares the hotspot.

#include "app/melee/Draw.hpp"
#include "app/melee/Game.hpp"

#include "engine/core/Geometry.hpp"
#include "engine/core/Types.hpp"
#include "game/Melee.hpp"
#include "game/SpriteSet.hpp"
#include "platform/Platform.hpp"
#include "sim/Battle.hpp"
#include "sim/Element.hpp"
#include "sim/Ship.hpp"
#include "sim/World.hpp"

#include <algorithm>
#include <array>
#include <string_view>

namespace uqm::melee {

namespace {

// A colour packed as 0xRRGGBB, the form the C's own tables are quoted in.
constexpr Colour
rgb(u32 c) noexcept
{
	return Colour{static_cast<u8>((c >> 16) & 0xFF),
			static_cast<u8>((c >> 8) & 0xFF),
			static_cast<u8>(c & 0xFF)};
}

// visualFor's fallback colours, and drawHud's crew-number colour.
[[nodiscard]] Colour
colourFor(sim::ElementKind kind, i32 playerNr) noexcept
{
	switch (kind)
	{
		case sim::ElementKind::Ship:
			return playerNr == 0 ? Colour{0x40, 0xC0, 0xFF}
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

// A 3x5 digit font (one bit per pixel), not the game's real font --
// wiring FontDir in means atlases and kerning for two numbers per
// player. Scaffolding until the M2 status panel uses real fonts.
constexpr std::array<u16, 10> kDigits{{
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
	const u16 bits = kDigits[static_cast<usize>(digit)];
	for (int row = 0; row < 5; ++row)
	{
		for (int col = 0; col < 3; ++col)
		{
			// Most significant bit is the top-left pixel.
			const int bit = 14 - (row * 3 + col);
			if (((bits >> bit) & 1) == 0)
				continue;
			w.fillRect(Vec2i{at.x + col * scale, at.y + row * scale},
					Extent2u{static_cast<u32>(scale),
						static_cast<u32>(scale)},
					c.r, c.g, c.b);
		}
	}
}

// Right-aligned, so a number shrinking from 18 to 9 does not shift about.
void
drawNumber(platform::Platform &w, i32 value, Vec2i rightTop,
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

// The starfield: three planes (30/60/90 stars), each scrolling at
// 1/2^plane of the camera so nearer planes move faster (galaxy.c:37-44,
// 405-407); plane 0 is nearest -- biggest, brightest, fastest.
inline constexpr int kStarPlanes = 3;
inline constexpr std::array<int, kStarPlanes> kStarsPerPlane{{30, 60, 90}};

// Fallback only, for when the art is missing: the cels carry their own
// colour and brightness already graded by plane, which is why they draw
// as frames rather than silhouettes.
inline constexpr std::array<Colour, kStarPlanes> kStarColours{{
		rgb(0x949CFC),
		rgb(0x808CFC),
		rgb(0xA4ACFC),
}};

// Cel per plane -- 11x11 near, 5x5 mid, a pixel far, one size down from
// star_frame_ofs's ordering (galaxy.c:316): the 11x11 cel is too large
// for a background at this resolution.
inline constexpr std::array<usize, kStarPlanes> kStarCels{{0, 2, 2}};

// cycle_ion_trail's colour table (tactrans.c:757-770), 5-bit RGB widened
// to 8: yellow-white at the muzzle through red to near-black, one step
// per frame.
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

// The Ilwrath cloak ramp, converted from the C's 5-bit RGB
// (ilwrath.c:255-275). Index 0 is unused: level 0 draws the real sprite,
// and the invisible last step is omitted.
constexpr std::array<Colour, 6> kCloakRamp{{
		rgb(0xFFFFFF),  // unused; level 0 draws the sprite
		rgb(0xFFFFFF),  // 1F,1F,1F
		rgb(0x52FFFF),  // 0A,1F,1F
		rgb(0x00A5A5),  // 00,14,14
		rgb(0x5252FF),  // 0A,0A,1F
		rgb(0x0000A5),  // 00,00,14
}};

}  // namespace

Visual
visualFor(Game &g, sim::ElementKind kind, i32 playerNr)
{
	const Colour fallback = colourFor(kind, playerNr);
	const Visual rect{nullptr, CelPolicy::Rect, fallback};

	// A missing or invalid set draws as Rect instead.
	const auto sprite = [&](std::string_view id, CelPolicy policy) {
		const game::SpriteSet &set = g.content.sprites(g.window, id);
		return set.valid() ? Visual{&set, policy, fallback} : rect;
	};

	// The owner's definition, for the kinds whose art is the ship's own.
	const game::ShipDef *def = playerNr >= 0
					&& static_cast<usize>(playerNr) < g.roster.size()
			? g.roster[static_cast<usize>(playerNr)]
			: nullptr;

	switch (kind)
	{
		case sim::ElementKind::Ship:
			return def != nullptr ? sprite(def->art.ship, CelPolicy::ByFacing)
								  : rect;
		case sim::ElementKind::ShipShadow:
			// A warp-in shadow is the ship's own image, so it borrows the
			// art, drawn tinted rather than plain.
			return def != nullptr
					? sprite(def->art.ship, CelPolicy::RampSilhouette)
					: rect;
		case sim::ElementKind::Weapon:
			return def != nullptr
					? sprite(def->art.weapon, CelPolicy::ByFrame)
					: rect;
		case sim::ElementKind::Laser:
			return Visual{nullptr, CelPolicy::BeamLine, fallback};
		case sim::ElementKind::IonTrail:
			return Visual{nullptr, CelPolicy::RampPoint, fallback};
		case sim::ElementKind::Debris:
			// Not validated here: the DebrisFrames draw branch checks
			// frames itself and falls back to its own fixed colour.
			return Visual{&g.content.sprites(g.window, game::kMeleeArt.boom),
					CelPolicy::DebrisFrames, fallback};
		case sim::ElementKind::Blast:
			// Weapon blasts and asteroid debris share a kind but not art. The
			// rubble an asteroid leaves is unowned; a blast belongs to the
			// shot that made it.
			return sprite(playerNr < 0 ? game::kMeleeArt.boom
									   : game::kMeleeArt.blast,
					CelPolicy::ByFacing);
		case sim::ElementKind::Asteroid:
			return sprite(game::kMeleeArt.asteroid, CelPolicy::ByFacing);
		case sim::ElementKind::Planet:
			return sprite(game::kMeleeArt.planet, CelPolicy::ByFacing);
		default:
			return rect;
	}
}

namespace {

// The background, before anything in the arena: stars do not zoom, since
// they are meant to be at infinity. Each plane is a screen-sized torus;
// the camera's position enters divided by 2^plane, a plain integer pan.
void
drawStars(Game &g)
{
	const Vec2i centre = g.camera.centre();

	const auto wrapTo = [](i32 v, i32 n) {
		v %= n;
		return v < 0 ? v + n : v;
	};

	usize first = 0;
	for (int plane = 0; plane < kStarPlanes; ++plane)
	{
		const auto p = static_cast<usize>(plane);
		const usize count = static_cast<usize>(kStarsPerPlane[p]);
		const Colour c = kStarColours[p];

		// World to display is a shift of two; the plane's own slowdown is
		// another `plane` on top of it.
		const i32 ox = centre.x >> (sim::kOneShift + plane);
		const i32 oy = centre.y >> (sim::kOneShift + plane);

		const game::SpriteSet *art =
				&g.content.sprites(g.window, game::kMeleeArt.stars);
		const usize cel = kStarCels[p];
		const bool haveArt = art != nullptr && cel < art->frames.size()
				&& cel < art->masks.size();

		Extent2u size{1, 1};
		Vec2i hs{0, 0};
		if (haveArt)
		{
			size = art->masks[cel].size();
			hs = art->masks[cel].hotspot();
		}

		for (usize i = first; i < first + count; ++i)
		{
			const i32 sx = wrapTo(g.stars[i].x - ox, kStarFieldWidth);
			const i32 sy = wrapTo(g.stars[i].y - oy, kStarFieldHeight);

			// Drawn up to four times so a star straddling the seam comes back
			// in on the other side instead of popping out of existence.
			for (int wx = 0; wx < 2; ++wx)
			{
				for (int wy = 0; wy < 2; ++wy)
				{
					const i32 x = sx - wx * kStarFieldWidth - hs.x;
					const i32 y = sy - wy * kStarFieldHeight - hs.y;
					if (x + static_cast<i32>(size.w) <= 0
							|| y + static_cast<i32>(size.h) <= 0
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

	// Every position goes through the camera's wrapDelta: the arena is a
	// torus eight screens across, so an element past the seam reads as a
	// few pixels away, not eight screens -- get it wrong and it jumps.
	g.battle.eachOrdered([&g](sim::EntityId id) {
		auto e = g.battle.get(id);
		if (e == nullptr)
			return;

		// Should not happen: every live element is attached in setUpBattle
		// or from a SpawnEvent. Safe fallback if one slipped through.
		const Visual *v = g.battle.find<Visual>(id);
		Visual missing;
		if (v == nullptr)
		{
			const sim::Allegiance *a = g.battle.find<sim::Allegiance>(id);
			missing = Visual{nullptr, CelPolicy::Rect,
					colourFor(e->kind, a != nullptr ? a->playerNr : -1)};
			v = &missing;
		}

		// A beam is a line between two points, not a sprite at one -- and,
		// alone among elements, has no Position at all (review-007 W4a): its
		// ends live in Beam{from,to}, read before the general `pos` fetch
		// below, which a beam would fail.
		if (v->policy == CelPolicy::BeamLine)
		{
			const sim::Beam *beam = g.battle.find<sim::Beam>(id);
			if (beam == nullptr)
				return;
			g.window.drawLine(g.camera.toScreen(beam->from),
					g.camera.toScreen(beam->to), 0xFF, 0xFF, 0xFF);
			return;
		}

		const sim::Position *pos = g.battle.find<sim::Position>(id);

		// Exhaust: a single point stepping through the colour ramp as it
		// ages. Lifetime::remaining counts down, so the ramp index counts up.
		if (v->policy == CelPolicy::RampPoint)
		{
			const usize step = static_cast<usize>(
					sim::kIonTrailLife - sim::lifeSpanOf(g.battle, id));
			if (step >= kIonRamp.size())
				return;
			const Colour c = kIonRamp[step];
			const Vec2i at = g.camera.toScreen(pos->current);
			g.window.fillRect(at, Extent2u{1, 1}, c.r, c.g, c.b);
			return;
		}

		// A spark of a dying ship: the boom animation, stepped by its own age
		// rather than by a facing, since it has none.
		if (v->policy == CelPolicy::DebrisFrames)
		{
			const game::SpriteSet *set = v->sprites;
			const Vec2i at = g.camera.toScreen(pos->current);
			if (set == nullptr || set->frames.empty())
			{
				g.window.fillRect(at, Extent2u{2, 2}, 0xFF, 0xC0, 0x40);
				return;
			}
			const usize frames = set->frames.size();
			const i32 age = sim::kDebrisLife - sim::lifeSpanOf(g.battle, id);
			const usize i = std::min(frames - 1,
					static_cast<usize>(std::max(0, age))
							* frames / static_cast<usize>(
									sim::kDebrisLife));
			const Extent2u sz = set->masks[i].size();
			const Vec2i hs = set->masks[i].hotspot();
			const i32 dw =
					std::max(1, g.camera.scale(sim::displayToWorld(
											 static_cast<i32>(sz.w))));
			const i32 dh =
					std::max(1, g.camera.scale(sim::displayToWorld(
											 static_cast<i32>(sz.h))));
			g.window.draw(set->frames[i],
					Vec2i{at.x - static_cast<i32>(hs.x) * dw
									/ std::max(1, static_cast<i32>(sz.w)),
						at.y - static_cast<i32>(hs.y) * dh
									/ std::max(1, static_cast<i32>(sz.h))},
					Extent2u{static_cast<u32>(dw),
						static_cast<u32>(dh)});
			return;
		}

		const Vec2i at = g.camera.toScreen(pos->current);

		// A weapon draws the cel AnimFrame names -- the facing for a
		// directional missile, the animation frame for the flame. Worked out
		// before the size below, which needs it too.
		const game::SpriteSet *set = v->sprites;
		const sim::AnimFrame *anim = g.battle.find<sim::AnimFrame>(id);
		const usize cel = set != nullptr
				? (v->policy == CelPolicy::ByFrame
								? static_cast<usize>(anim != nullptr ? anim->n : 0)
										% set->frames.size()
								: static_cast<usize>(pos->facing.raw())
										% set->frames.size())
				: 0;

		// One zoom, one scale, no separate LOD path (design-notes.md D6).
		// Size comes from content's own mask -- the same CollisionMask a
		// Collider would borrow, reached through the SpriteSet instead of
		// the entity, so a NonSolid element (no Collider at all) still
		// draws at its real size. No sprite at all keeps the old fallback.
		const Extent2u mask =
				set != nullptr ? set->masks[cel].size() : Extent2u{8, 8};
		const i32 w = std::max(
				1, g.camera.scale(sim::displayToWorld(
						   static_cast<i32>(mask.w))));
		const i32 h = std::max(
				1, g.camera.scale(sim::displayToWorld(
						   static_cast<i32>(mask.h))));

		if (at.x + w < 0 || at.y + h < 0 || at.x - w > sim::kSpaceWidth
				|| at.y - h > sim::kSpaceHeight)
			return;

		const Extent2u dest{static_cast<u32>(w),
				static_cast<u32>(h)};

		// A ship that has not arrived yet is not drawn -- only the shadows it
		// sheds are (tactrans.c:863). And a dead one is drawn as its own
		// explosion, growing over the frames it burns for. Keyed off the
		// ship component itself, since only a Ship element ever has one.
		if (const sim::ShipState *s = g.battle.ship(id); s != nullptr)
		{
			const i32 crew = s->crew;

			if (!g.battle.has<sim::Collider>(id) && crew > 0)
				return;  // still warping in

			// A dying ship keeps its own hull for the first fifteen frames,
			// then stops drawing (tactrans.c:569-571); the explosion itself
			// is the swarm of sparks explosionPreProcess spawns as Debris.
			if (crew == 0
					&& sim::kExplosionLife - sim::lifeSpanOf(g.battle, id)
							>= sim::kHullVanishAge)
				return;
		}

		if (set != nullptr)
		{
			// Draw from the cel's hotspot, not its centre: each facing's
			// hotspot differs (the ship is asymmetric), and centring would
			// shift the hull as it turns and offset it from the mask.
			const Vec2i hs = set->masks[cel].hotspot();
			const Extent2u src = set->masks[cel].size();
			const i32 ox = src.w != 0
					? static_cast<i32>(hs.x) * w
							/ static_cast<i32>(src.w)
					: w / 2;
			const i32 oy = src.h != 0
					? static_cast<i32>(hs.y) * h
							/ static_cast<i32>(src.h)
					: h / 2;

			// A warp-in shadow: the hull as a flat fill stepping through
			// the exhaust ramp as it ages -- the same flat-fill technique
			// the cloak below uses, since only the outline is wanted.
			if (v->policy == CelPolicy::RampSilhouette)
			{
				const usize step = static_cast<usize>(
						sim::kIonTrailLife - sim::lifeSpanOf(g.battle, id));
				if (step >= kIonRamp.size() || cel >= set->silhouettes.size())
					return;
				const Colour c = kIonRamp[step];
				g.window.drawTinted(set->silhouettes[cel],
						Vec2i{at.x - ox, at.y - oy}, dest, c.r, c.g, c.b);
				return;
			}

			const sim::Cloak *cloakState = g.battle.find<sim::Cloak>(id);
			const i32 cloak = cloakState != nullptr ? cloakState->level : 0;
			if (cloak > 0 && cel < set->silhouettes.size())
			{
				// Cloak ramp (ilwrath.c:250-285): levels 1..5 are the fill
				// colours; kCloakFullLevel is BLACK, which the C fills but
				// which here would show as a hole against the dark clear.
				if (cloak >= sim::kCloakFullLevel)
					return;
				const Colour c = kCloakRamp[static_cast<usize>(cloak)];
				g.window.drawTinted(set->silhouettes[cel],
						Vec2i{at.x - ox, at.y - oy}, dest, c.r, c.g, c.b);
				return;
			}

			g.window.draw(set->frames[cel], Vec2i{at.x - ox, at.y - oy}, dest);
			return;
		}

		const Colour c = v->fallback;
		g.window.fillRect(Vec2i{at.x - w / 2, at.y - h / 2}, dest, c.r, c.g,
				c.b);
	});

	drawHud(g);

	if (g.debugOverlay)
		drawOverlay(g);

	g.window.present();
}

namespace {

// Crew and energy per player, in the corner each owns, coloured to match
// the ship. Drawn from the element itself, so a destroyed ship shows
// nothing rather than a stale number -- which is how "dead" reads.
void
drawHud(Game &g)
{
	constexpr int kScale = 1;
	constexpr i32 kMargin = 4;
	constexpr i32 kLine = 7;

	for (usize p = 0; p < g.ships.size(); ++p)
	{
		const auto e = g.battle.get(g.ships[p]);
		if (e == nullptr)
			continue;
		const sim::ShipState *s = g.battle.ship(g.ships[p]);
		if (s == nullptr)
			continue;

		const auto *a = g.battle.find<sim::Allegiance>(g.ships[p]);
		const Colour crewColour = colourFor(e->kind, a != nullptr ? a->playerNr : -1);
		constexpr Colour energyColour{0x60, 0xFF, 0xC0};

		// Player 0 hugs the left edge, player 1 the right. Both are drawn
		// right-aligned; only the anchor differs.
		const i32 right = p == 0
				? kMargin + 3 * 4 * kScale
				: sim::kSpaceWidth - kMargin;

		drawNumber(g.window, s->crew, Vec2i{right, kMargin}, kScale,
				crewColour);
		drawNumber(g.window, s->energy, Vec2i{right, kMargin + kLine},
				kScale, energyColour);
	}
}

// The collision overlay: what touched, where, and the response. Reads
// Battle::collisions() rather than recomputing anything, so what is drawn
// is exactly what the simulation resolved.
void
drawOverlay(Game &g)
{
	// Mask bounds, so it is visible when a silhouette is not what you expect.
	// Reads the Collider now, not the entity: an element without one has no
	// collision box to show, which is the truthful picture (review-007 W2).
	g.battle.eachOrdered([&g](sim::EntityId id) {
		const auto e = g.battle.get(id);
		const sim::Collider *c = g.battle.find<sim::Collider>(id);
		if (e == nullptr || c == nullptr)
			return;

		const sim::Position *pos = g.battle.find<sim::Position>(id);
		const Vec2i at = g.camera.toScreen(pos->current);
		const Extent2u m = c->mask->size();
		const i32 w = g.camera.scale(
				sim::displayToWorld(static_cast<i32>(m.w)));
		const i32 h = g.camera.scale(
				sim::displayToWorld(static_cast<i32>(m.h)));
		const Vec2i hs = c->mask->hotspot();
		const i32 ox = m.w != 0
				? static_cast<i32>(hs.x) * w
						/ static_cast<i32>(m.w)
				: w / 2;
		const i32 oy = m.h != 0
				? static_cast<i32>(hs.y) * h
						/ static_cast<i32>(m.h)
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
	});

	// Contact points and response vectors. Velocities are scaled up because a
	// frame of travel is a handful of world units and an unscaled arrow would
	// be a dot.
	constexpr i32 kVectorGain = 8;
	for (const Game::Mark &mark : g.marks)
	{
		const i64 age = static_cast<i64>(g.battle.frame()) - mark.frame;
		if (age > kMarkLife)
			continue;

		const Vec2i at = g.camera.toScreen(mark.event.at);
		g.window.drawLine(Vec2i{at.x - 4, at.y - 4}, Vec2i{at.x + 4, at.y + 4},
				0xFF, 0x30, 0x30);
		g.window.drawLine(Vec2i{at.x - 4, at.y + 4}, Vec2i{at.x + 4, at.y - 4},
				0xFF, 0x30, 0x30);

		// Before in dim, after in bright: the difference *is* the response.
		const auto arrow = [&](Vec2i v, u8 r, u8 gg,
								   u8 b) {
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
