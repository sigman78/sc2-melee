// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Battle.hpp"

#include "engine/core/Types.hpp"
#include "sim/Damage.hpp"
#include "sim/Gravity.hpp"
#include "sim/Impulse.hpp"
#include "sim/ShipSystems.hpp"
#include "sim/World.hpp"

#include <algorithm>
#include <utility>

namespace uqm::sim {

namespace {

// The silhouette/facing an element ENTERED the frame with, captured before
// any hook runs: the overlap-repair protocol (process.c:453-506) reverts to
// these to undo a rotation that turned the element into a wall. Anonymous
// namespace on purpose -- this is the protocol's own scratch, and a
// component type only this file can name is as private as C++ gets
// (review-004 X5's worked example of a split with a real ownership win).
struct PriorSilhouette
{
	static constexpr auto in_place_delete = true;

	Borrowed<const CollisionMask> mask = nullptr;
	Facing facing;
};

// CollisionPossible (collide.h:34-39): skips a pair when both are already
// stopped, when both carry IGNORE_SIMILAR and share an owner (both, not
// either; owner, not player or kind), or when neither side has mass.
// Solidity itself (testCollidable) is the caller's gate now, checked before
// any of Motion/Physique/CollisionScratch is even fetched (review-007
// W4b) -- by the time this runs, the test side is known collidable. Owner
// is the only Element/Allegiance field this ever needed, so it takes the
// raw ids rather than either struct.
[[nodiscard]] bool
collisionPossible(EntityId testOwner, const Physique &testPhys,
		EntityId elemOwner, const Physique &elemPhys, bool testCollided,
		bool elemCollided, bool testIgnoreSimilar,
		bool elemIgnoreSimilar) noexcept
{
	if (testCollided && elemCollided)
		return false;
	if (testIgnoreSimilar && elemIgnoreSimilar && testOwner != kNoEntity
			&& testOwner == elemOwner)
		return false;
	if (testPhys.mass == 0 && elemPhys.mass == 0)
		return false;
	return true;
}

// Masks are display pixels, positions are world units -- the conversion
// isn't optional, or the intersect test sees everything 4x further apart.
// The C converts at this exact boundary (collide.h:44-54).
[[nodiscard]] Body
bodyOf(const Position &pos, const CollisionMask *mask) noexcept
{
	return Body{mask, worldToDisplay(pos.current), worldToDisplay(pos.next)};
}

// The entity's current mask, or null if it has no Collider -- Collider took
// over from the field Element::mask used to be (review-007 W2).
[[nodiscard]] const CollisionMask *
maskOf(const entt::registry &reg, EntityId id) noexcept
{
	const Collider *c = reg.try_get<Collider>(id);
	return c != nullptr ? c->mask : nullptr;
}

// Where a body stops, rewound to the impact time in world units, not
// converted back from display space -- see design-notes D3 (V2).
[[nodiscard]] Vec2i
rewindTo(Vec2i from, Vec2i to, TimeValue time) noexcept
{
	const i32 t = static_cast<i32>(time) - 1;  // 0..256
	return Vec2i{from.x
				+ static_cast<i32>((i64{to.x - from.x} * t) >> kTimeShift),
		from.y
				+ static_cast<i32>((i64{to.y - from.y} * t) >> kTimeShift)};
}

// In world units per frame, so consumers never see the packed fixed point.
[[nodiscard]] Vec2i
worldVelocityOf(const Motion &m) noexcept
{
	const Vec2i v = m.velocity.current();
	return Vec2i{velocityToWorld(v.x), velocityToWorld(v.y)};
}

}  // namespace

i32
lifeSpanOf(const Battle &b, EntityId id) noexcept
{
	const Lifetime *l = b.find<Lifetime>(id);
	return l != nullptr ? l->remaining : 1;
}

bool
isFiniteLife(const Battle &b, EntityId id) noexcept
{
	return b.find<Lifetime>(id) != nullptr;
}

Battle::Battle(u32 seed) : rng_(seed)
{
	// One chunk was the whole battle in the old arena (EntityList's
	// kChunkSize); the reserve keeps the steady-state step allocation-free.
	reg_.storage<Element>().reserve(64);
	reg_.storage<Position>().reserve(64);
	reg_.storage<Motion>().reserve(64);
	reg_.storage<Physique>().reserve(64);
	reg_.storage<Order>().reserve(64);
	reg_.storage<PriorSilhouette>().reserve(64);
	reg_.storage<CollisionScratch>().reserve(64);
	collideOrder_.reserve(64);
}

bool
Battle::collidable(EntityId id) const noexcept
{
	const Element *e = get(id);
	return e != nullptr && reg_.all_of<Collider>(id)
			&& !reg_.all_of<Doomed>(id);
}

void
Battle::removeElement(EntityId id) noexcept
{
	if (!alive(id))
		return;

	// Bumps the entity's version, which is what turns a surviving handle
	// into a detectable mistake instead of a read of the next tenant.
	reg_.destroy(id);
	--count_;
}

void
Battle::buildOrderedIds(std::vector<EntityId> &out) const noexcept
{
	out.clear();
	for (EntityId id : reg_.view<const Element>())
		out.push_back(id);
	std::sort(out.begin(), out.end(), [this](EntityId a, EntityId b) {
		const Order &oa = reg_.get<const Order>(a);
		const Order &ob = reg_.get<const Order>(b);
		if (oa.layer != ob.layer)
			return oa.layer < ob.layer;
		return oa.seq < ob.seq;
	});
}

void
Battle::recordSpawn(EntityId id, const Element &e, const Allegiance &allegiance)
{
	spawns_.push_back(SpawnEvent{id, e.kind, allegiance.playerNr});
}

void
Battle::queueSpawn(SpawnCommand cmd)
{
	spawnCommands_.push_back(std::move(cmd));
}

ShipState *
Battle::ship(EntityId id) noexcept
{
	return reg_.valid(id) ? reg_.try_get<ShipState>(id) : nullptr;
}

const ShipState *
Battle::ship(EntityId id) const noexcept
{
	return reg_.valid(id) ? reg_.try_get<const ShipState>(id) : nullptr;
}

ShipState &
Battle::attachShip(EntityId id, Borrowed<const ShipSpec> spec)
{
	ShipState &s = reg_.emplace<ShipState>(id);
	s.spec = spec;
	reg_.emplace<Input>(id);
	return s;
}

Borrowed<const WeaponSpec>
Battle::weaponSpec(EntityId id) const noexcept
{
	const auto *g =
			reg_.valid(id) ? reg_.try_get<const WeaponGuidance>(id) : nullptr;
	return g != nullptr ? g->spec : nullptr;
}

void
Battle::attachWeaponSpec(EntityId id, Borrowed<const WeaponSpec> spec)
{
	reg_.emplace<WeaponGuidance>(id, spec);
}

EntityId
Battle::spawn(Layer layer, Element e, Position pos, Motion motion,
		Physique physique, Borrowed<const CollisionMask> collider,
		Allegiance allegiance)
{
	const EntityId id = reg_.create();
	recordSpawn(id, e, allegiance);

	// Seeded from the mask the element is spawning with, not left null: a
	// mid-pipeline spawn (a death hook's replacement asteroid) reaches
	// Collide with no CapturePrior pass of its own to have filled this in,
	// and a null mask reads as "turned" against anything it overlaps,
	// letting the overlap-repair protocol assign that null mask onto a live
	// element. Attaching the Collider here too, in the same call, is what
	// keeps this seed and the component in agreement -- a caller attaching
	// one a statement later would leave this reading stale.
	const PriorSilhouette prior{collider, pos.facing};
	reg_.emplace<Element>(id, std::move(e));
	reg_.emplace<Position>(id, pos);
	reg_.emplace<Motion>(id, motion);
	reg_.emplace<Physique>(id, physique);
	reg_.emplace<Allegiance>(id, allegiance);
	reg_.emplace<PriorSilhouette>(id, prior);
	reg_.emplace<CollisionScratch>(id);
	reg_.emplace<Order>(id, Order{layer, nextSeq_++});
	reg_.emplace<Appearing>(id);
	if (collider != nullptr)
		reg_.emplace<Collider>(id, Collider{collider});
	++count_;
	return id;
}

EntityId
Battle::spawnBeam(Layer layer, Element e, Beam beam, Allegiance allegiance)
{
	const EntityId id = reg_.create();
	recordSpawn(id, e, allegiance);

	// The minimal-composition rule's worked example (review-007 W4b): a
	// beam is never solid (no Collider ever attaches to one) and never
	// moves, so Motion/Physique/PriorSilhouette/CollisionScratch would be
	// dead weight -- resolveAgainst gates on collidable(testId) before
	// touching any of them, so a beam never needs the scaffold to exist.
	// Appearing drops too: nothing that reads it can ever reach a beam
	// (every reader is gated behind Position, PlayerShip/WarpingIn, or
	// collidable(), none of which a beam has). Order and Allegiance stay --
	// a beam is still walked and drawn like anything else, and Allegiance
	// is the one uniform attach.
	reg_.emplace<Element>(id, std::move(e));
	reg_.emplace<Beam>(id, beam);
	reg_.emplace<Allegiance>(id, allegiance);
	reg_.emplace<Order>(id, Order{layer, nextSeq_++});
	++count_;
	return id;
}

// "BAD NEWS": an APPEARING element wedged inside something on spawn dies
// on the spot (process.c:427-449) -- full damage, then DISAPPEARING with
// its death hook run now (hit_points is crew for a ship, element.h:126-133).
void
Battle::killOverlapSpawn(EntityId id)
{
	auto e = get(id);
	if (e == nullptr)
		return;

	const ShipState *s = ship(id);
	const Vitality *v = find<Vitality>(id);
	doDamage(*this, id, s != nullptr ? s->crew : v != nullptr ? v->hitPoints : 0);
	e = get(id);
	if (e == nullptr)
		return;
	reg_.get<CollisionScratch>(id).collided = true;
	reg_.emplace_or_replace<Doomed>(id);
	if (e->onDeath != nullptr)
		e->onDeath(*this, id);
}

// One candidate pair, from first possibility to full resolution -- the body
// of the C's ProcessCollisions loop (process.c:382-621). Returns whether the
// scanner is done for this walk (the C's mid-loop `return COLLISION`).
bool
Battle::resolveAgainst(EntityId elemId, usize elemIdx, EntityId testId,
		usize testIdx, TimeValue maxTime)
{
	auto e = get(elemId);
	if (e == nullptr)
		return true;
	auto t = get(testId);
	if (t == nullptr)
		return false;

	// Solidity first, before any of Motion/Physique/CollisionScratch is
	// fetched (review-007 W4b's minimal-composition rule): elemId is
	// already known collidable (the caller's invariant -- collidePass only
	// starts a walk from a collidable id), but testId is not, and a
	// candidate with no Collider -- a beam, once the diet dropped its
	// physics scaffold (Battle::spawnBeam) -- carries none of the rest
	// either. Gating here is what keeps the blanket fetches below from
	// reading through that hole.
	if (!collidable(testId))
		return false;

	// Held for the rest of this call: nothing spawns or is destroyed mid-Collide
	// (see processCollisions), so neither pool moves under these references.
	CollisionScratch &eScratch = reg_.get<CollisionScratch>(elemId);
	CollisionScratch &tScratch = reg_.get<CollisionScratch>(testId);
	Position *ePos = reg_.try_get<Position>(elemId);
	Position *tPos = reg_.try_get<Position>(testId);
	Motion *eMotion = reg_.try_get<Motion>(elemId);
	Motion *tMotion = reg_.try_get<Motion>(testId);
	Physique *ePhys = reg_.try_get<Physique>(elemId);
	Physique *tPhys = reg_.try_get<Physique>(testId);

	if (!collisionPossible(reg_.get<Allegiance>(testId).owner, *tPhys,
			reg_.get<Allegiance>(elemId).owner, *ePhys, tScratch.collided,
			eScratch.collided, reg_.all_of<IgnoreSimilar>(testId),
			reg_.all_of<IgnoreSimilar>(elemId)))
		return false;

	// A transient element doesn't collide on its spawn frame (process.c:389-394)
	// -- exempts FINITE_LIFE-with-Appearing on EITHER side, so a missile can't
	// detonate on its own muzzle; lifeSpan > 1 still lets one-frame PD fire.
	if ((isFiniteLife(*this, elemId) || isFiniteLife(*this, testId))
			&& ((reg_.all_of<Appearing>(elemId) && lifeSpanOf(*this, elemId) > 1)
					|| (reg_.all_of<Appearing>(testId)
							&& lifeSpanOf(*this, testId) > 1)))
		return false;

	const bool bothSolid =
			!(isFiniteLife(*this, elemId) || isFiniteLife(*this, testId));
	Impact hit = sweptIntersect(bodyOf(*ePos, maskOf(reg_, elemId)),
			bodyOf(*tPos, maskOf(reg_, testId)), maxTime);

	// "BAD NEWS" (process.c:397-516): impact at time 1 between two solids is a
	// standing overlap, not a new collision -- a repair protocol, not a
	// response, or responding again welds ships together. Weapons are exempt.
	while (hit.time == 1 && bothSolid)
	{
		if (eScratch.collided)
		{
			// The scanner already stopped this frame; the overlap is real
			// only if it persists with the test element taken at its END
			// position (process.c:405-413).
			const Body still{maskOf(reg_, testId), worldToDisplay(tPos->next),
					worldToDisplay(tPos->next)};
			hit = sweptIntersect(bodyOf(*ePos, maskOf(reg_, elemId)), still, 1);
			if (hit.time != 1)
				break;
		}

		const PriorSilhouette &ePrior = reg_.get<PriorSilhouette>(elemId);
		const PriorSilhouette &tPrior = reg_.get<PriorSilhouette>(testId);
		const bool eTurned = maskOf(reg_, elemId) != ePrior.mask;
		const bool tTurned = maskOf(reg_, testId) != tPrior.mask;
		if (!eTurned && !tTurned)
		{
			// Neither silhouette changed: either a spawn wedged inside something
			// (dies on the spot), or the tail of an already-resolved contact, skipped
			// so the original impulse can carry the pair apart (process.c:427-451, 509-515).
			if (reg_.all_of<Appearing>(testId))
				killOverlapSpawn(testId);
			if (reg_.all_of<Appearing>(elemId))
			{
				killOverlapSpawn(elemId);
				return true;
			}
			hit = Impact{};
			break;
		}

		// A silhouette changed into the overlap -- something rotated into a wall.
		// Undo the turn (process.c:453-506) and ask again: the old silhouette may
		// find no contact, a genuine impact, or a standing overlap settled above.
		// A null prior reverts to no Collider at all, not one with a null mask
		// -- Collider's presence is what collidable() reads, so a mask left
		// null would misread as still solid.
		if (eTurned)
		{
			if (ePrior.mask != nullptr)
				reg_.get<Collider>(elemId).mask = ePrior.mask;
			else
				reg_.remove<Collider>(elemId);
			ePos->facing = ePrior.facing;
		}
		if (tTurned)
		{
			if (tPrior.mask != nullptr)
				reg_.get<Collider>(testId).mask = tPrior.mask;
			else
				reg_.remove<Collider>(testId);
			tPos->facing = tPrior.facing;
		}
		hit = sweptIntersect(bodyOf(*ePos, maskOf(reg_, elemId)),
				bodyOf(*tPos, maskOf(reg_, testId)), maxTime);
	}

	if (!hit)
		return false;

	const Vec2i elemStop = rewindTo(ePos->current, ePos->next, hit.time);
	const Vec2i testStop = rewindTo(tPos->current, tPos->next, hit.time);

	// Earliest-collision-wins (process.c:531-540): before resolving at
	// `hit.time`, recursively resolve whether either side hits something
	// earlier first; a yes on either side abandons this pair. Scanner's side first.
	if (hit.time != 1)
	{
		const auto earlier = static_cast<TimeValue>(hit.time - 1);

		if (!eScratch.collided
				&& processCollisions(elemId, elemIdx, testIdx + 1, earlier))
			return false;
		e = get(elemId);
		t = get(testId);
		ePos = reg_.try_get<Position>(elemId);
		tPos = reg_.try_get<Position>(testId);
		eMotion = reg_.try_get<Motion>(elemId);
		tMotion = reg_.try_get<Motion>(testId);
		ePhys = reg_.try_get<Physique>(elemId);
		tPhys = reg_.try_get<Physique>(testId);
		if (e == nullptr)
			return true;
		if (t == nullptr)
			return false;

		if (!tScratch.collided)
		{
			// The C scans the test element's earlier candidates from the
			// scanner's successor -- or from the head when the test element
			// is newly spawned (process.c:535-540).
			const usize from =
					reg_.all_of<Appearing>(testId) ? 0 : elemIdx + 1;
			if (processCollisions(testId, testIdx, from, earlier))
				return false;
		}
		e = get(elemId);
		t = get(testId);
		ePos = reg_.try_get<Position>(elemId);
		tPos = reg_.try_get<Position>(testId);
		eMotion = reg_.try_get<Motion>(elemId);
		tMotion = reg_.try_get<Motion>(testId);
		ePhys = reg_.try_get<Physique>(elemId);
		tPhys = reg_.try_get<Physique>(testId);
		if (e == nullptr)
			return true;
		if (t == nullptr)
			return false;
	}

	// Resolution. The hooks decide who stops -- each raises Collided on
	// itself, exactly as the C's collision_funcs raise COLLISION -- and run
	// ship-side first when the TEST element is the ship (process.c:549-570).
	const bool elemHad = eScratch.collided;
	const bool testHad = tScratch.collided;
	const bool bothSolidNow =
			!(isFiniteLife(*this, elemId) || isFiniteLife(*this, testId));

	e->collidedWith = testId;
	t->collidedWith = elemId;

	CollisionEvent event;
	event.a = elemId;
	event.b = testId;
	event.at = elemStop;
	event.beforeA = worldVelocityOf(*eMotion);
	event.beforeB = worldVelocityOf(*tMotion);

	const ElementHook eHook = e->onCollision;
	const ElementHook tHook = t->onCollision;
	if (reg_.all_of<PlayerShip>(testId))
	{
		if (tHook != nullptr)
			tHook(*this, testId);
		if (eHook != nullptr && get(elemId) != nullptr)
			eHook(*this, elemId);
	}
	else
	{
		if (eHook != nullptr)
			eHook(*this, elemId);
		if (tHook != nullptr && get(testId) != nullptr)
			tHook(*this, testId);
	}

	e = get(elemId);
	t = get(testId);
	ePos = reg_.try_get<Position>(elemId);
	tPos = reg_.try_get<Position>(testId);
	eMotion = reg_.try_get<Motion>(elemId);
	tMotion = reg_.try_get<Motion>(testId);
	ePhys = reg_.try_get<Physique>(elemId);
	tPhys = reg_.try_get<Physique>(testId);

	// Whoever NEWLY raised Collided stops at the impact point
	// (process.c:572-596); a side that was already stopped keeps the
	// position its first collision gave it.
	if (t != nullptr && tScratch.collided && !testHad)
		tPos->next = testStop;

	bool impulsed = false;
	if (e != nullptr && eScratch.collided && !elemHad)
	{
		ePos->next = elemStop;

		// Momentum exchange is solid-on-solid only (process.c:598-601). A
		// weapon hit is resolved by damage, from the collidedWith set above.
		if (t != nullptr && bothSolidNow)
		{
			// Pure physics is told who is a ship by a ShipState pointer, null
			// for anything else (review-007 W4b: the turn/thrust stagger is
			// ShipState's own field now, not a flag Impulse reads off Element).
			applyImpulse(*ePos, *eMotion, *ePhys, ship(elemId), eScratch,
					*tPos, *tMotion, *tPhys, ship(testId), tScratch);
			impulsed = true;

			// collide.c:104-110: an impulse invalidates the at-max bookkeeping.
			if (reg_.all_of<PlayerShip>(elemId))
				if (ShipState *ss = ship(elemId))
					ss->speed = SpeedState::Normal;
			if (reg_.all_of<PlayerShip>(testId))
				if (ShipState *ss = ship(testId))
					ss->speed = SpeedState::Normal;
		}
	}

	event.afterA = e != nullptr ? worldVelocityOf(*eMotion) : event.beforeA;
	event.afterB = t != nullptr ? worldVelocityOf(*tMotion) : event.beforeB;
	collisions_.push_back(event);

	if (impulsed)
	{
		// Both participants immediately re-scan the whole list
		// (process.c:603-606): a pile-up chains within this frame instead of
		// resolving one pair per step.
		processCollisions(elemId, elemIdx, 0, kMaxTimeValue);
		processCollisions(testId, testIdx, 0, kMaxTimeValue);
	}

	// Keeps scanning unless out of the game for the frame (process.c:609-618):
	// stopped, or no longer collidable -- a ship merely hit by a missile is
	// neither, and can still bounce off another ship this frame.
	e = get(elemId);
	if (e == nullptr || eScratch.collided)
		return true;
	if (!collidable(elemId))
	{
		eScratch.collided = true;
		return true;
	}
	return false;
}

// ProcessCollisions (process.c:361-627): walks candidates from `fromIdx` in
// collideOrder_. Returns whether `elem` ended the walk stopped. Z4 drops the
// straggler preprocessing the C interleaved here (process.c:371-373):
// Integrate (pipeline slot 8) already ran over every element before Collide
// ever starts, so there is nothing left unintegrated for this walk to catch
// up. Nothing is destroyed mid-Collide, so an index into collideOrder_,
// once taken, never moves out from under this walk (see collidePass).
bool
Battle::processCollisions(
		EntityId elemId, usize elemIdx, usize fromIdx, TimeValue maxTime)
{
	for (usize idx = fromIdx; idx < collideOrder_.size();)
	{
		const EntityId testId = collideOrder_[idx];
		if (get(testId) == nullptr)
			break;

		const usize succIdx = idx + 1;

		if (!(testId == elemId)
				&& resolveAgainst(elemId, elemIdx, testId, idx, maxTime))
			return true;

		idx = succIdx;
	}

	auto e = get(elemId);
	return e == nullptr || reg_.get<CollisionScratch>(elemId).collided;
}

// CapturePrior (pipeline slot 1): the silhouette/facing every element enters
// the frame with, batched at frame start -- the same meaning the per-entity
// capture had (it ran right before that entity's own hook), since nothing
// before Collide (slot 9) can change a mask or a facing anyway. Collided is
// cleared here too: cheapest done alongside a pass that already visits
// everyone, and it must read false before Collide sets it fresh this frame.
void
Battle::capturePriorPass() noexcept
{
	for (auto [id, pos, prior, scratch] :
			reg_.view<Position, PriorSilhouette, CollisionScratch>().each())
	{
		prior.mask = maskOf(reg_, id);
		prior.facing = pos.facing;
		scratch.collided = false;
	}
}

// AgeAndReap-mark (pipeline slot 2): the death check stays exactly where it
// was -- frame start, before anything else touches the element (process.c
// has no FINITE_LIFE guard: do_damage kills a non-aging asteroid by
// assigning life_span = 0 directly, misc.c:210,221). Only the CHECK moved
// here; the matching decrement is slot 11b, at the sync point.
//
// Stays a find<Lifetime> test over eachOrdered's walk, not a pool view over
// Lifetime: onDeath hooks draw RNG (the asteroid field's replacement, a
// dying ship's debris), so which order the deaths are discovered in is
// gameplay, not incidental -- the same reason this pass was never folded
// into a bare view<Lifetime>.
void
Battle::ageAndReapMarkPass()
{
	eachOrdered([this](EntityId id) {
		const Lifetime *life = reg_.try_get<Lifetime>(id);
		if (life == nullptr || life->remaining != 0)
			return;
		reg_.emplace<Doomed>(id);
		auto e = get(id);
		if (e != nullptr && e->onDeath != nullptr)
			e->onDeath(*this, id);
	});
}

// Animate (pipeline slot 7): what is left of the per-element preProcess hook
// dispatch once ships (ShipMachines/Turn/Thrust, ShipSystems.cpp) and Guided
// shots (GuidedSteer) have their own passes -- the flame's frame-advance,
// the asteroid's tumble. Appearing still suppresses the hook for one frame,
// exactly as it always has (a weapon's hook does not run until its second
// frame alive). eachOrdered's emission order equals the retired spine's, so
// sim_test.cpp's testStepVisitsInListOrder (an earlier-layer element must
// animate first) is clean under it too.
void
Battle::animatePass()
{
	eachOrdered([this](EntityId id) {
		auto e = get(id);
		if (e != nullptr && !reg_.all_of<PlayerShip>(id)
				&& !reg_.all_of<Guided>(id) && e->preProcess != nullptr
				&& !reg_.all_of<Appearing>(id))
		{
			e->preProcess(*this, id);
		}
	});
}

// Integrate (pipeline slot 8): SetUpElement's seeding (process.c:117-126)
// plus the motion add (process.c:163,172-173), batched over the whole spine
// instead of interleaved per entity. The wrap still happens at Commit
// (slot 12), not here -- design-notes D4.
void
Battle::integratePass() noexcept
{
	// A beam has no Position (review-007 W4a), so this view never sees one --
	// the old BeamGeometry exemption (seeding next from current would have
	// collapsed a beam to a point) is gone with it, not replaced.
	for (auto [id, pos, mot] : reg_.view<Position, Motion>().each())
	{
		if (reg_.all_of<IgnoreVelocity>(id))
			continue;

		if (reg_.all_of<Appearing>(id))
			pos.next = pos.current;

		const Vec2i delta = mot.velocity.advance(1);
		pos.next = Vec2i{pos.next.x + delta.x, pos.next.y + delta.y};
	}
}

// Collide (pipeline slot 9): the same pair-walk machinery as always
// (processCollisions/resolveAgainst), now running over a spine that is
// already fully integrated -- no straggler preprocessing needed, and no
// catch-up pass, because nothing spawned this frame exists yet (that is
// slot 11d). onCollision hooks still run inline; ship crew damage they
// cause now lands in DamageIncoming instead of applying on the spot (see
// Damage.cpp), applied at the sync point below.
//
// Z5: collideOrder_ is a snapshot of every live element, sorted ascending
// by (Order.layer, Order.seq). Nothing is destroyed mid-Collide (the reap is
// a later sync point), so an index into this snapshot stays valid for the
// rest of the pass, and processCollisions/resolveAgainst walk it by index.
void
Battle::collidePass()
{
	buildOrderedIds(collideOrder_);

	for (usize i = 0; i < collideOrder_.size(); ++i)
	{
		const EntityId id = collideOrder_[i];
		if (collidable(id) && !reg_.get<CollisionScratch>(id).collided)
		{
			// Successors only, so each pair is visited once per frame.
			(void)processCollisions(id, i, i + 1, kMaxTimeValue);
		}
	}
}

// Sync point, 11a: one summed deltaCrew and one death check per victim,
// same semantics as today's doDamage on a crewed hull (review-006 §2's
// second Z4 refinement) -- just applied once here instead of on whichever
// hit landed first.
void
Battle::applyDamageIncoming() noexcept
{
	reg_.view<DamageIncoming, ShipState>().each(
			[this](EntityId id, DamageIncoming &di, ShipState &s) {
				if (!deltaCrew(s, -di.amount))
					startShipExplosion(*this, id);
			});
	reg_.clear<DamageIncoming>();
}

// AgeDecrement: today's per-entity decrement runs inside preProcessOne,
// after that entity's own hook (ShipMachines/Turn/Thrust for a ship,
// Animate/GuidedSteer for anything else) and after its own integration, but
// before ANY collision testing touches it -- called from step() at exactly
// that seam, between Integrate and Collide, rather than batched in at the
// sync point the doc's slot list groups it with. Two things pin it there,
// both found by running the suite:
//
// - Too early (right after the slot 2 death check) double-counts a ship's
//   own warp-in: ShipMachines is what attaches Lifetime{kWarpInFrames} on
//   the appearing frame, and that has to happen before a decrement can
//   apply to it, or the first traced frame is off by one (kWarpInFrames
//   instead of kWarpInFrames - 1, caught by replay_test's --trace diff
//   against the pre-Z4 baseline).
// - Too late (at the sync point, after Collide/Fire) lets a same-frame kill
//   that sets Lifetime{0} without Doomed -- doDamage's non-ship branch,
//   used by a piercing pair where only one side's own weaponCollision
//   fall-through sets Doomed, and by the PD special killing a shot
//   outright -- get decremented straight past zero to -1 this same frame.
//   A remaining that never lands back on exactly 0 is never detected as
//   dead next frame; testOpposingMissilesDestroyEachOther and
//   testPointDefenceBurnsOwnNuke both failed this way before landing here.
//
// view<Lifetime>, not view<Element>: only a Lifetime holder ages. The
// planet has none (Indestructible instead), so it is invisible to this
// view entirely -- no per-entity exemption needed. Doomed is excluded in
// the query itself (SiGMan's review), not an in-body has<> guard: presence/
// absence belongs in the view, value tests (there are none left here) stay
// in the body.
void
Battle::ageDecrementPass() noexcept
{
	reg_.view<Lifetime>(entt::exclude<Doomed>)
			.each([](Lifetime &life) { --life.remaining; });
}

// Sync point, 11c: destroy every element already Doomed -- marked in slot 2
// from a death detected this frame, or mid-Collide (slot 9) by a weapon's
// own self-spend or the overlap-repair protocol's overlap-kill. Either way
// its onDeath already ran at the point Doomed was set; nothing runs again
// here, exactly as postProcessPass's reap branch never called postProcess
// either.
//
// view<Doomed>, not view<Element> filtered by a flag: a bare tag view walks
// its own pool order, not (layer, seq) -- a different destruction order
// than the old Disappearing-filtered walk over Element. Nothing here reads
// order: no hook runs (onDeath already ran), and destruction touches only
// the destroyed entities' own slots, never a survivor's Order stamp -- so
// which order they're destroyed in is not gameplay, only entity-id
// recycling, and that is not folded into the replay digest either.
void
Battle::reapPass() noexcept
{
	// Element is in_place_delete, so removing the CURRENT entity mid-walk
	// is safe -- entt leaves later slots undisturbed, no separate
	// next-before-remove capture needed the way the spine walk required.
	for (EntityId id : reg_.view<Doomed>())
		removeElement(id);
}

// Sync point, 11e: end-of-frame flag housekeeping over the spine as it
// stands BEFORE 11d creates this frame's spawns (run first here for
// exactly that reason -- see drainSpawnCommands). A newborn must keep
// Appearing through its own first live frame, which under Z4 is next
// frame's pipeline, not a same-frame catch-up; clearing it now would be one
// frame early.
//
// A plain view over CollisionScratch, not eachOrdered: each entity's own
// scratch is independent of every other's, and CollisionScratch's presence
// IS the filter -- a beam has none (review-007 W4b's diet), so a view
// excludes it structurally instead of a per-entity null check catching it.
void
Battle::flagsEndOfFramePass() noexcept
{
	// A frame without a collision ends DefyPhysics (process.c:824-827): it
	// has to expire, or the first stationary contact disables the collision
	// stagger for good.
	reg_.view<CollisionScratch>().each([](CollisionScratch &scratch) {
		if (!scratch.collided)
			scratch.defyPhysics = false;
	});
	reg_.clear<Appearing>();
}

// Sync point, 11d: create every entity the frame's pipeline asked for, in
// emission order -- deterministic because the pipeline that filled the
// buffer is. Run after 11e (see above) so a fresh spawn's own Appearing
// survives to its first real frame instead of being stripped the instant
// it is born.
void
Battle::drainSpawnCommands()
{
	for (SpawnCommand &cmd : spawnCommands_)
	{
		if (cmd.deferred != nullptr)
		{
			// Its own RNG draws happen here, in queue order -- not at
			// emission, which would put them out of step with this same
			// sync point's other draws.
			cmd.deferred(*this, cmd.deferredMask);
			continue;
		}

		// A beam has no Position, so it takes its own spawn entry point
		// entirely -- cmd.position/motion/physique are never read for one
		// (review-007 W4a).
		const EntityId id = cmd.beam
				? spawnBeam(cmd.layer, std::move(cmd.element), *cmd.beam,
						  cmd.allegiance)
				: spawn(cmd.layer, std::move(cmd.element), cmd.position,
						  cmd.motion, cmd.physique, cmd.collider,
						  cmd.allegiance);
		if (cmd.weaponSpec != nullptr)
			attachWeaponSpec(id, cmd.weaponSpec);
		if (cmd.guided)
			attach<Guided>(id, *cmd.guided);
		if (cmd.lifetime)
			attach<Lifetime>(id, *cmd.lifetime);
		if (cmd.vitality)
			attach<Vitality>(id, *cmd.vitality);
		if (cmd.warhead)
			attach<Warhead>(id, *cmd.warhead);
		if (cmd.animFrame)
			attach<AnimFrame>(id, *cmd.animFrame);
		if (cmd.ignoreVelocity)
			attach<IgnoreVelocity>(id);
		if (cmd.ignoreSimilar)
			attach<IgnoreSimilar>(id);
		if (cmd.rubbleMask != nullptr)
			attach<StashedMask>(id, StashedMask{cmd.rubbleMask});
	}
	spawnCommands_.clear();
}

// Commit (pipeline slot 12): the wrap and the publish, unchanged from
// today's postProcess commit (process.c:899-916) except that it now runs as
// its own whole-spine pass instead of once per entity inline with its hook.
// A beam has no Position (review-007 W4a), so this view never sees one --
// the old BeamGeometry exemption is gone with it, not replaced.
void
Battle::commitPass() noexcept
{
	for (auto [id, pos] : reg_.view<Position>().each())
	{
		pos.next = wrap(pos.next);
		pos.current = pos.next;
	}
}

void
Battle::step()
{
	collisions_.clear();
	spawns_.clear();

	capturePriorPass();
	ageAndReapMarkPass();
	energyRegenPass(*this);
	shipMachinesPass(*this);
	turnPass(*this);
	thrustPass(*this);
	guidedSteerPass(*this);
	gravityPass(*this);
	animatePass();
	integratePass();
	ageDecrementPass();
	collidePass();
	fireAndSpecialGatePass(*this);

	applyDamageIncoming();
	reapPass();
	flagsEndOfFramePass();
	drainSpawnCommands();

	commitPass();

	++frame_;
}

}  // namespace uqm::sim
