// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.
//
// Ships, projectiles, asteroids and the planet draw from content; anything
// without art yet falls back to a coloured rectangle, deliberately ugly so
// it reads as missing rather than as a choice.
//
// Positioning is by each cel's hotspot, not its centre -- facings are not
// symmetric and each sits differently in its box, so centring would make
// the hull wander and drift off the collision mask, which shares the hotspot.
//
// The pipeline (review-007 §2): clear -> renderStars -> renderPlanet ->
// renderAsteroids -> renderShips -> renderProjectiles -> renderEffects ->
// marks -> hud -> overlay(ctx-gated) -> present. Each pass is keyed by the
// components/tags that identify its content -- CelPolicy is gone, the pass
// IS the policy now, and Visual carries only a sprite set and a fallback
// colour. Z-order is (pass, seq), declared entirely by the order below;
// Layer (Entity.hpp) stays a purely sim-side ordering concept.

#include "app/melee/Draw.hpp"
#include "app/melee/Game.hpp"

#include "engine/core/Geometry.hpp"
#include "engine/core/Types.hpp"
#include "game/Camera.hpp"
#include "game/Melee.hpp"
#include "game/SpriteSet.hpp"
#include "platform/Platform.hpp"
#include "sim/Battle.hpp"
#include "sim/Element.hpp"
#include "sim/Field.hpp"
#include "sim/Ship.hpp"
#include "sim/World.hpp"

#include <algorithm>
#include <array>
#include <string_view>

namespace uqm::melee {

namespace {

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

// -- Shared placement math, every sprite-backed pass draws from a cel's
// hotspot at the camera's current scale (file header). --------------------

// True if a mask-sized sprite at screen point `at` would land fully off the
// visible arena -- the coarse AABB cull every sprite-backed pass shares.
[[nodiscard]] bool
offScreen(Vec2i at, i32 w, i32 h) noexcept
{
	return at.x + w < 0 || at.y + h < 0 || at.x - w > sim::kSpaceWidth
			|| at.y - h > sim::kSpaceHeight;
}

// Scales a mask's display-pixel size to the camera's current zoom, floored
// at one screen pixel each axis so a distant cel never disappears entirely.
[[nodiscard]] Extent2u
scaledSize(const game::Camera &camera, Extent2u mask) noexcept
{
	const i32 w = std::max(1,
			camera.scale(sim::displayToWorld(static_cast<i32>(mask.w))));
	const i32 h = std::max(1,
			camera.scale(sim::displayToWorld(static_cast<i32>(mask.h))));
	return Extent2u{static_cast<u32>(w), static_cast<u32>(h)};
}

// The cel's hotspot, scaled the same as its destination size, as a top-left
// offset from the screen point the hotspot sits at -- draw-from-hotspot
// instead of draw-from-centre (see file header).
[[nodiscard]] Vec2i
hotspotOffset(Vec2i hotspot, Extent2u mask, Extent2u dest) noexcept
{
	const i32 ox = mask.w != 0
			? static_cast<i32>(hotspot.x) * static_cast<i32>(dest.w)
					/ static_cast<i32>(mask.w)
			: static_cast<i32>(dest.w) / 2;
	const i32 oy = mask.h != 0
			? static_cast<i32>(hotspot.y) * static_cast<i32>(dest.h)
					/ static_cast<i32>(mask.h)
			: static_cast<i32>(dest.h) / 2;
	return Vec2i{ox, oy};
}

// The plain by-facing draw every non-ship sprite (planet, asteroid, blast)
// shares: cel = facing.raw() % frames, hotspot-anchored, rect fallback when
// there is no art. renderShips does not use this -- it also has warp gating,
// hull-vanish and cloak tinting layered on top.
void
drawFacingSprite(Game &g, const sim::Position &pos, const Visual &v)
{
	const game::Camera &camera = g.battle.context<game::Camera>();
	const Vec2i at = camera.toScreen(pos.current);
	const game::SpriteSet *set = v.sprites;
	const usize cel = set != nullptr
			? static_cast<usize>(pos.facing.raw()) % set->frames.size()
			: 0;
	const Extent2u maskSize = set != nullptr ? set->masks[cel].size()
											  : Extent2u{8, 8};
	const Extent2u dest = scaledSize(camera, maskSize);
	if (offScreen(at, static_cast<i32>(dest.w), static_cast<i32>(dest.h)))
		return;

	if (set == nullptr)
	{
		g.window.fillRect(Vec2i{at.x - static_cast<i32>(dest.w) / 2,
								 at.y - static_cast<i32>(dest.h) / 2},
				dest, v.fallback.r, v.fallback.g, v.fallback.b);
		return;
	}
	const Vec2i off = hotspotOffset(set->masks[cel].hotspot(), maskSize, dest);
	g.window.draw(set->frames[cel], Vec2i{at.x - off.x, at.y - off.y}, dest);
}

}  // namespace

Visual
visualFor(Game &g, sim::EntityId id, i32 playerNr)
{
	sim::Battle &b = g.battle;

	// The owner's definition, for the art that is the ship's own (the ship
	// itself, and the shadow it sheds while warping in, which borrows it).
	const auto &roster = b.context<BattleConfig>().roster;
	const game::ShipDef *def = playerNr >= 0
					&& static_cast<usize>(playerNr) < roster.size()
			? roster[static_cast<usize>(playerNr)]
			: nullptr;

	// A missing or invalid set draws as a plain rectangle instead.
	const auto sprite = [&](std::string_view art, Colour fallback) {
		const game::SpriteSet &set = g.content.sprites(g.window, art);
		return set.valid() ? Visual{&set, fallback} : Visual{nullptr, fallback};
	};

	// Composition selects the art now (review-007 W7): what an element draws
	// as follows from what it is composed of, not a kind field -- the tags
	// this keys on (Planet/Trail/Shadow/Debris/Blast/Spin, plus Cloaked in
	// renderShips) are attached at each one's own spawn site.

	if (b.has<sim::ShipState>(id) || b.has<sim::Shadow>(id))
		return def != nullptr
				? sprite(def->art.ship,
						playerNr == 0 ? Colour{0x40, 0xC0, 0xFF}
									  : Colour{0xFF, 0x60, 0x40})
				: Visual{nullptr, Colour{0xC0, 0xC0, 0xC0}};

	if (b.has<sim::Beam>(id))
		return Visual{nullptr, Colour{0xC0, 0xC0, 0xC0}};

	if (b.has<sim::Warhead>(id))
		return def != nullptr
				? sprite(def->art.weapon, Colour{0xFF, 0xE0, 0x60})
				: Visual{nullptr, Colour{0xFF, 0xE0, 0x60}};

	if (b.has<sim::Trail>(id))
		return Visual{nullptr, Colour{0xC0, 0xC0, 0xC0}};

	if (b.has<sim::Debris>(id))
		return sprite(game::kMeleeArt.boom, Colour{0xC0, 0xC0, 0xC0});

	if (b.has<sim::Blast>(id))
		return sprite(playerNr < 0 ? game::kMeleeArt.boom : game::kMeleeArt.blast,
				Colour{0xFF, 0xFF, 0xC0});

	if (b.has<sim::Spin>(id))
		return sprite(game::kMeleeArt.asteroid, Colour{0x90, 0x88, 0x78});

	if (b.has<sim::Planet>(id))
		return sprite(game::kMeleeArt.planet, Colour{0x60, 0x90, 0x50});

	return Visual{nullptr, Colour{0xC0, 0xC0, 0xC0}};
}

namespace {

// The background, before anything in the arena: stars do not zoom, since
// they are meant to be at infinity. Each plane is a screen-sized torus;
// the camera's position enters divided by 2^plane, a plain integer pan.
// Keyed on the Starfield component: one entity, no Position, no Order
// (review-007 §3) -- an unordered view rather than eachOrdered, matching
// that.
void
renderStars(Game &g)
{
	const game::Camera &camera = g.battle.context<game::Camera>();
	const Vec2i centre = camera.centre();

	const auto wrapTo = [](i32 v, i32 n) {
		v %= n;
		return v < 0 ? v + n : v;
	};

	g.battle.view<Starfield>().each([&](sim::EntityId, Starfield &sf) {
		usize first = 0;
		for (int plane = 0; plane < kStarPlanes; ++plane)
		{
			const auto p = static_cast<usize>(plane);
			const StarPlane &sp = kStarsPerPlane[p];
			const usize count = static_cast<usize>(sp.count);
			const Colour c = sp.colour;

			// World to display is a shift of two; the plane's own slowdown is
			// another `plane` on top of it.
			const i32 ox = centre.x >> (sim::kOneShift + plane);
			const i32 oy = centre.y >> (sim::kOneShift + plane);

			const game::SpriteSet *art =
					&g.content.sprites(g.window, game::kMeleeArt.stars);
			const usize cel = sp.cel;
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
				const i32 sx = wrapTo(sf.stars[i].x - ox, kStarFieldWidth);
				const i32 sy = wrapTo(sf.stars[i].y - oy, kStarFieldHeight);

				// Drawn up to four times so a star straddling the seam comes
				// back in on the other side instead of popping out of
				// existence.
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
	});
}

// The battlefield's one gravity well, keyed on the Planet tag.
void
renderPlanet(Game &g)
{
	g.battle.eachOrdered<sim::Planet, sim::Position, Visual>(
			[&g](sim::EntityId, sim::Position &pos, Visual &v) {
				drawFacingSprite(g, pos, v);
			});
}

// Spin already identifies the asteroid field (attached since review-007 W5,
// nothing new here) -- the tag review-006 owed anyway.
void
renderAsteroids(Game &g)
{
	g.battle.eachOrdered<sim::Position, sim::Spin, Visual>(
			[&g](sim::EntityId, sim::Position &pos, sim::Spin &, Visual &v) {
				drawFacingSprite(g, pos, v);
			});
}

// renderShips owns the whole ship look now (review-007 W7): facing sprite,
// cloak tint, warp gating -- the multi-technique-per-entity edge the old
// CelPolicy switch used to straddle dissolves into one pass's internal
// order (hull, then the cloak/shadow tint layered on top).
void
renderShips(Game &g)
{
	g.battle.eachOrdered<sim::Position, sim::ShipState, Visual>(
			[&g](sim::EntityId id, sim::Position &pos, sim::ShipState &s,
					Visual &v) {
				// A ship that has not arrived yet is not drawn -- only the
				// shadows it sheds are (tactrans.c:863).
				if (!g.battle.has<sim::Collider>(id) && s.crew > 0)
					return;

				// A dying ship keeps its own hull for the first fifteen
				// frames, then stops drawing (tactrans.c:569-571); the
				// explosion itself is the swarm of sparks renderEffects
				// draws as Debris.
				if (s.crew == 0
						&& sim::Exploding::kLife - sim::lifeSpanOf(g.battle, id)
								>= sim::Exploding::kHullVanishAge)
					return;

				const game::Camera &camera = g.battle.context<game::Camera>();
				const Vec2i at = camera.toScreen(pos.current);
				const game::SpriteSet *set = v.sprites;
				const usize cel = set != nullptr
						? static_cast<usize>(pos.facing.raw())
								% set->frames.size()
						: 0;
				const Extent2u maskSize =
						set != nullptr ? set->masks[cel].size() : Extent2u{8, 8};
				const Extent2u dest = scaledSize(camera, maskSize);
				if (offScreen(
							at, static_cast<i32>(dest.w), static_cast<i32>(dest.h)))
					return;

				if (set == nullptr)
				{
					g.window.fillRect(Vec2i{at.x - static_cast<i32>(dest.w) / 2,
											 at.y - static_cast<i32>(dest.h) / 2},
							dest, v.fallback.r, v.fallback.g, v.fallback.b);
					return;
				}

				const Vec2i off =
						hotspotOffset(set->masks[cel].hotspot(), maskSize, dest);
				const Vec2i tl{at.x - off.x, at.y - off.y};

				// Cloak tint (ilwrath.c:250-285): Cloaked is the full-black
				// invisible step (the tag holds iff level is full); the 1..5
				// tint ramp reads Cloak.level directly.
				if (const sim::Cloak *cloak = g.battle.find<sim::Cloak>(id))
				{
					if (g.battle.has<sim::Cloaked>(id))
						return;
					const i32 level = cloak->level;
					if (level > 0 && static_cast<usize>(level) < kCloakRamp.size()
							&& cel < set->silhouettes.size())
					{
						const Colour c = kCloakRamp[static_cast<usize>(level)];
						g.window.drawTinted(set->silhouettes[cel], tl, dest,
								c.r, c.g, c.b);
						return;
					}
				}

				g.window.draw(set->frames[cel], tl, dest);
			});
}

// Warhead identifies a weapon shot in flight; a beam has no Position at all
// (review-007 W4a) and is drawn as a line between its own two ends instead.
// Shots draw before beams -- a declared choice; both keep their own seq
// order, but the two groups are no longer globally interleaved by spawn
// order the way one combined walk used to leave them.
void
renderProjectiles(Game &g)
{
	g.battle.eachOrdered<sim::Position, sim::Warhead, Visual>(
			[&g](sim::EntityId id, sim::Position &pos, sim::Warhead &, Visual &v) {
				const game::Camera &camera = g.battle.context<game::Camera>();
				const Vec2i at = camera.toScreen(pos.current);
				const game::SpriteSet *set = v.sprites;
				const sim::AnimFrame *anim = g.battle.find<sim::AnimFrame>(id);

				// A weapon draws the cel AnimFrame names -- the facing for a
				// directional missile, the animation frame for the flame.
				const usize cel = set != nullptr
						? static_cast<usize>(anim != nullptr ? anim->n : 0)
								% set->frames.size()
						: 0;
				const Extent2u maskSize =
						set != nullptr ? set->masks[cel].size() : Extent2u{8, 8};
				const Extent2u dest = scaledSize(camera, maskSize);
				if (offScreen(
							at, static_cast<i32>(dest.w), static_cast<i32>(dest.h)))
					return;

				if (set == nullptr)
				{
					g.window.fillRect(Vec2i{at.x - static_cast<i32>(dest.w) / 2,
											 at.y - static_cast<i32>(dest.h) / 2},
							dest, v.fallback.r, v.fallback.g, v.fallback.b);
					return;
				}
				const Vec2i off =
						hotspotOffset(set->masks[cel].hotspot(), maskSize, dest);
				g.window.draw(set->frames[cel], Vec2i{at.x - off.x, at.y - off.y},
						dest);
			});

	g.battle.eachOrdered<sim::Beam>([&g](sim::EntityId, sim::Beam &beam) {
		const game::Camera &camera = g.battle.context<game::Camera>();
		g.window.drawLine(camera.toScreen(beam.from), camera.toScreen(beam.to),
				0xFF, 0xFF, 0xFF);
	});
}

// The Trail/Shadow/Debris/Blast tags, in that declared order -- a stacking
// choice: the old code interleaved all four by spawn order in one combined
// walk. Each keeps its own seq order within its pass.
void
renderEffects(Game &g)
{
	const game::Camera &camera = g.battle.context<game::Camera>();

	// Ion trail: a single point stepping through the colour ramp as it ages.
	// Lifetime::remaining counts down, so the ramp index counts up.
	g.battle.eachOrdered<sim::Trail, sim::Position>(
			[&g, &camera](sim::EntityId id, sim::Position &pos) {
				const usize step = static_cast<usize>(
						sim::Trail::kLife - sim::lifeSpanOf(g.battle, id));
				if (step >= kIonRamp.size())
					return;
				const Colour c = kIonRamp[step];
				g.window.fillRect(
						camera.toScreen(pos.current), Extent2u{1, 1}, c.r, c.g, c.b);
			});

	// Warp-in shadow: the hull as a flat fill stepping through the same
	// ramp -- the ship's own art, borrowed via Visual, drawn as a tinted
	// silhouette instead of the sprite.
	g.battle.eachOrdered<sim::Shadow, sim::Position, Visual>(
			[&g, &camera](sim::EntityId id, sim::Position &pos, Visual &v) {
				const game::SpriteSet *set = v.sprites;
				if (set == nullptr)
					return;
				const usize step = static_cast<usize>(
						sim::Trail::kLife - sim::lifeSpanOf(g.battle, id));
				const usize cel =
						static_cast<usize>(pos.facing.raw()) % set->frames.size();
				if (step >= kIonRamp.size() || cel >= set->silhouettes.size())
					return;
				const Extent2u maskSize = set->masks[cel].size();
				const Extent2u dest = scaledSize(camera, maskSize);
				const Vec2i at = camera.toScreen(pos.current);
				if (offScreen(
							at, static_cast<i32>(dest.w), static_cast<i32>(dest.h)))
					return;
				const Vec2i off =
						hotspotOffset(set->masks[cel].hotspot(), maskSize, dest);
				const Colour c = kIonRamp[step];
				g.window.drawTinted(set->silhouettes[cel],
						Vec2i{at.x - off.x, at.y - off.y}, dest, c.r, c.g, c.b);
			});

	// A spark of a dying ship: the boom animation, stepped by its own age
	// rather than by a facing, since it has none.
	g.battle.eachOrdered<sim::Debris, sim::Position, Visual>(
			[&g, &camera](sim::EntityId id, sim::Position &pos, Visual &v) {
				const Vec2i at = camera.toScreen(pos.current);
				const game::SpriteSet *set = v.sprites;
				if (set == nullptr || set->frames.empty())
				{
					g.window.fillRect(at, Extent2u{2, 2}, 0xFF, 0xC0, 0x40);
					return;
				}
				const usize frames = set->frames.size();
				const i32 age = sim::Debris::kLife - sim::lifeSpanOf(g.battle, id);
				const usize i = std::min(frames - 1,
						static_cast<usize>(std::max(0, age)) * frames
								/ static_cast<usize>(sim::Debris::kLife));
				const Extent2u maskSize = set->masks[i].size();
				const Extent2u dest = scaledSize(camera, maskSize);
				const Vec2i off =
						hotspotOffset(set->masks[i].hotspot(), maskSize, dest);
				g.window.draw(
						set->frames[i], Vec2i{at.x - off.x, at.y - off.y}, dest);
			});

	// Blast: a weapon's impact flash, or the rubble an asteroid leaves --
	// both draw by facing, same as the planet and the asteroid field.
	g.battle.eachOrdered<sim::Blast, sim::Position, Visual>(
			[&g](sim::EntityId, sim::Position &pos, Visual &v) {
				drawFacingSprite(g, pos, v);
			});
}

// Contact points and response vectors, gated by DebugToggles.overlay (F1) --
// the same single gate the old drawOverlay used for both halves of the
// debug view; this is the half keyed on Mark entities (review-007 §3), an
// app-owned bare walk (an unordered view, not eachOrdered: Mark carries no
// Order).
void
renderMarks(Game &g)
{
	if (!g.battle.context<DebugToggles>().overlay)
		return;

	const game::Camera &camera = g.battle.context<game::Camera>();
	constexpr i32 kVectorGain = 8;

	g.battle.view<Mark>().each([&](sim::EntityId, Mark &mark) {
		const Vec2i at = camera.toScreen(mark.event.at);
		g.window.drawLine(Vec2i{at.x - 4, at.y - 4}, Vec2i{at.x + 4, at.y + 4},
				0xFF, 0x30, 0x30);
		g.window.drawLine(Vec2i{at.x - 4, at.y + 4}, Vec2i{at.x + 4, at.y - 4},
				0xFF, 0x30, 0x30);

		// Before in dim, after in bright: the difference *is* the response.
		const auto arrow = [&](Vec2i v, u8 r, u8 gg, u8 b) {
			g.window.drawLine(at,
					Vec2i{at.x + camera.scale(v.x * kVectorGain),
						at.y + camera.scale(v.y * kVectorGain)},
					r, gg, b);
		};
		arrow(mark.event.a.before, 0x60, 0x60, 0x90);
		arrow(mark.event.b.before, 0x60, 0x60, 0x90);
		arrow(mark.event.a.after, 0x60, 0xC0, 0xFF);
		arrow(mark.event.b.after, 0xFF, 0xC0, 0x60);
	});
}

// Crew and energy per player, in the corner each owns, coloured to match
// the ship. Drawn straight from ShipState, so a destroyed ship shows
// nothing rather than a stale number -- which is how "dead" reads. The
// player index doubles as the roster index, so no per-entity lookup (kind
// or Allegiance) is needed to pick the colour.
void
renderHud(Game &g)
{
	constexpr int kScale = 1;
	constexpr i32 kMargin = 4;
	constexpr i32 kLine = 7;
	constexpr Colour energyColour{0x60, 0xFF, 0xC0};
	constexpr std::array<Colour, 2> kCrewColours{
			{{0x40, 0xC0, 0xFF}, {0xFF, 0x60, 0x40}}};

	const auto &shipIds = g.battle.context<MatchState>().shipIds;
	for (usize p = 0; p < shipIds.size(); ++p)
	{
		const sim::ShipState *s = g.battle.ship(shipIds[p]);
		if (s == nullptr)
			continue;

		// Player 0 hugs the left edge, player 1 the right. Both are drawn
		// right-aligned; only the anchor differs.
		const i32 right = p == 0 ? kMargin + 3 * 4 * kScale
								 : sim::kSpaceWidth - kMargin;

		drawNumber(g.window, s->crew, Vec2i{right, kMargin}, kScale,
				kCrewColours[p]);
		drawNumber(g.window, s->energy, Vec2i{right, kMargin + kLine}, kScale,
				energyColour);
	}
}

// The collision overlay: what touched, where. Reads the Collider, not the
// entity: an element without one has no collision box to show, which is
// the truthful picture (review-007 W2). Gated by the same DebugToggles.
void
renderOverlay(Game &g)
{
	if (!g.battle.context<DebugToggles>().overlay)
		return;

	const game::Camera &camera = g.battle.context<game::Camera>();
	g.battle.eachOrdered<sim::Position, sim::Collider>(
			[&g, &camera](sim::EntityId, sim::Position &pos, sim::Collider &c) {
				const Vec2i at = camera.toScreen(pos.current);
				const Extent2u m = c.mask->size();
				const i32 w =
						camera.scale(sim::displayToWorld(static_cast<i32>(m.w)));
				const i32 h =
						camera.scale(sim::displayToWorld(static_cast<i32>(m.h)));
				const Vec2i hs = c.mask->hotspot();
				const i32 ox = m.w != 0
						? static_cast<i32>(hs.x) * w / static_cast<i32>(m.w)
						: w / 2;
				const i32 oy = m.h != 0
						? static_cast<i32>(hs.y) * h / static_cast<i32>(m.h)
						: h / 2;

				const Vec2i tl{at.x - ox, at.y - oy};
				const Vec2i br{tl.x + w, tl.y + h};
				g.window.drawLine(tl, Vec2i{br.x, tl.y}, 0x30, 0x60, 0x30);
				g.window.drawLine(Vec2i{br.x, tl.y}, br, 0x30, 0x60, 0x30);
				g.window.drawLine(br, Vec2i{tl.x, br.y}, 0x30, 0x60, 0x30);
				g.window.drawLine(Vec2i{tl.x, br.y}, tl, 0x30, 0x60, 0x30);

				// The hotspot itself, which is where the game thinks the
				// thing *is*.
				g.window.drawLine(Vec2i{at.x - 2, at.y}, Vec2i{at.x + 2, at.y},
						0x40, 0xFF, 0x40);
				g.window.drawLine(Vec2i{at.x, at.y - 2}, Vec2i{at.x, at.y + 2},
						0x40, 0xFF, 0x40);
			});
}

}  // namespace

void
draw(Game &g)
{
	g.window.clear(0x08, 0x08, 0x18);

	// Every position goes through the camera's wrapDelta: the arena is a
	// torus eight screens across, so an element past the seam reads as a
	// few pixels away, not eight screens -- get it wrong and it jumps.
	renderStars(g);
	renderPlanet(g);
	renderAsteroids(g);
	renderShips(g);
	renderProjectiles(g);
	renderEffects(g);
	renderMarks(g);
	renderHud(g);
	renderOverlay(g);

	g.window.present();
}

}  // namespace uqm::melee
