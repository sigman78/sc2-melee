// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Battle.hpp"

#include "engine/core/Types.hpp"
#include "sim/Damage.hpp"
#include "sim/Field.hpp"
#include "sim/Gravity.hpp"
#include "sim/Impulse.hpp"
#include "sim/ShipSystems.hpp"
#include "sim/Specials.hpp"
#include "sim/World.hpp"

#include <cassert>
#include <tuple>
#include <utility>

namespace uqm::sim {

namespace comp::inline matter {
namespace {

// The silhouette/facing an element ENTERED the frame with, captured before
// any hook runs: the overlap-repair protocol (process.c:453-506) reverts to
// these to undo a rotation that turned the element into a wall. Private to
// this file: the repair protocol's own scratch, no reader outside it.
struct PriorSilhouette
{
	static constexpr auto in_place_delete = true;

	Borrowed<const CollisionMask> mask = nullptr;
	Facing facing;
};

}  // namespace
}  // namespace comp::inline matter

namespace {

// CollisionPossible (collide.h:34-39): skips a pair when both are stopped,
// both carry IGNORE_SIMILAR with a shared owner, or neither has mass.
// Takes raw ids -- owner is the only Allegiance field this needs.
[[nodiscard]] bool collisionPossible(EntityId testOwner,
		const comp::Physique &testPhys, EntityId elemOwner,
		const comp::Physique &elemPhys, bool testCollided, bool elemCollided,
		bool testIgnoreSimilar, bool elemIgnoreSimilar) noexcept
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
[[nodiscard]] Body bodyOf(
		const comp::Position &pos, const CollisionMask *mask) noexcept
{
	return Body{mask, worldToDisplay(pos.current), worldToDisplay(pos.next)};
}

[[nodiscard]] const CollisionMask *maskOf(
		const entt::registry &reg, EntityId id) noexcept
{
	const comp::Collider *c = reg.try_get<comp::Collider>(id);
	return c != nullptr ? c->mask : nullptr;
}

// Where a body stops, rewound to the impact time in world units, not
// converted back from display space -- see design-notes D3.
[[nodiscard]] Vec2i rewindTo(Vec2i from, Vec2i to, TimeValue time) noexcept
{
	const i32 t = static_cast<i32>(time) - 1;  // 0..256
	return Vec2i{
			from.x + static_cast<i32>((i64{to.x - from.x} * t) >> kTimeShift),
			from.y + static_cast<i32>((i64{to.y - from.y} * t) >> kTimeShift)};
}

// In world units per frame, so consumers never see the packed fixed point.
[[nodiscard]] Vec2i worldVelocityOf(const comp::Motion &m) noexcept
{
	const Vec2i v = m.velocity.current();
	return Vec2i{velocityToWorld(v.x), velocityToWorld(v.y)};
}

}  // namespace

bool isTransient(const Battle &b, EntityId id) noexcept
{
	return b.reg.all_of<comp::Lifetime>(id);
}

i32 framesLeft(const Battle &b, EntityId id) noexcept
{
	return b.reg.get<comp::Lifetime>(id).remaining;
}

i32 ageOf(const Battle &b, EntityId id, i32 span) noexcept
{
	return span - framesLeft(b, id);
}

Battle::Battle(u32 seed) : rng_(seed)
{
	// The sort key's own pool reports when the walk is stale, so no spawn or
	// destroy path has to remember to raise the flag.
	reg.on_construct<comp::Order>().connect<&Battle::markOrderDirty>(*this);
	reg.on_destroy<comp::Order>().connect<&Battle::markOrderDirty>(*this);

	// Keeps the steady-state step allocation-free.
	reg.storage<comp::Position>().reserve(64);
	reg.storage<comp::Motion>().reserve(64);
	reg.storage<comp::Physique>().reserve(64);
	reg.storage<comp::Order>().reserve(64);
	reg.storage<comp::PriorSilhouette>().reserve(64);
	reg.storage<comp::CollisionScratch>().reserve(64);
	collideOrder_.reserve(64);
}

bool Battle::collidable(EntityId id) const noexcept
{
	return alive(id) && reg.all_of<comp::Collider>(id)
			&& !reg.all_of<comp::Doomed>(id)
			&& !reg.all_of<comp::WarpingIn>(id);
}

void Battle::removeElement(EntityId id) noexcept
{
	if (!alive(id))
		return;

	// Bumps the entity's version, which is what turns a surviving handle
	// into a detectable mistake instead of a read of the next tenant. The
	// Order observer invalidates the sort on the way out.
	reg.destroy(id);
}

// Reached from inside an eachOrdered walk as well as before one, so the sort
// must never fire mid-frame -- it would permute the array the outer walk
// holds an iterator into. Order is emplaced only in landPendingSpawns and
// erased only in reapPass, both sync points.
void Battle::ensureOrdered()
{
	if (!orderDirty_)
		return;
	reg.sort<comp::Order>([](const comp::Order &a, const comp::Order &b) {
		return a.layer != b.layer ? a.layer < b.layer : a.seq < b.seq;
	});
	orderDirty_ = false;
}

// SpawnEvent::kind is derived from composition: Beam -> Laser, Warhead ->
// Weapon, the only two flavors Sound.cpp's dispatch uses. Must run after
// those components attach (spawn/spawnBeam call it last).
void Battle::recordSpawn(EntityId id, const comp::Allegiance &allegiance)
{
	SpawnFlavor flavor = SpawnFlavor::Unknown;
	if (reg.all_of<comp::Beam>(id))
		flavor = SpawnFlavor::Laser;
	else if (reg.all_of<comp::Warhead>(id))
		flavor = SpawnFlavor::Weapon;
	spawns_.push_back(SpawnEvent{id, flavor, allegiance.playerNr});
}

void Battle::queueDeferred(
		void (*fn)(Battle &, Borrowed<const CollisionMask>) noexcept,
		Borrowed<const CollisionMask> mask)
{
	pending_.push_back(PendingSpawn{kNoEntity, Layer::Field, fn, mask});
}

comp::Order Battle::nextOrder(Layer layer) noexcept
{
	return comp::Order{layer, nextSeq_++};
}

Spawned Battle::make(Layer layer)
{
	const EntityId id = reg.create();
	if (deferSpawns_)
		pending_.push_back(PendingSpawn{id, layer});
	else
		reg.emplace<comp::Order>(id, nextOrder(layer));
	return Spawned{*this, id};
}

// Recorded when the entity joins the walk, which is also when every
// component it will spawn with is attached -- so the flavor below reads a
// finished thing. A pending spawn is recorded at the landing instead.
void Battle::recordIfLanded(EntityId id, const comp::Allegiance &allegiance)
{
	if (reg.all_of<comp::Order>(id))
		recordSpawn(id, allegiance);
}

EntityId Battle::create()
{
	return reg.create();
}

void Battle::destroy(EntityId id) noexcept
{
	if (!reg.valid(id))
		return;
	// create() itself never attaches an Order, so this normally touches
	// nothing the walk cares about -- and if it ever does, the Order
	// observer covers it.
	reg.destroy(id);
}

comp::ShipState *Battle::ship(EntityId id) noexcept
{
	return reg.valid(id) ? reg.try_get<comp::ShipState>(id) : nullptr;
}

const comp::ShipState *Battle::ship(EntityId id) const noexcept
{
	return reg.valid(id) ? reg.try_get<const comp::ShipState>(id) : nullptr;
}

comp::ShipState &Battle::attachShip(EntityId id, Borrowed<const ShipSpec> spec)
{
	comp::ShipState &s = reg.emplace<comp::ShipState>(id);
	s.spec = spec;
	reg.emplace<comp::Input>(id);
	return s;
}

Borrowed<const WeaponSpec> Battle::weaponSpec(EntityId id) const noexcept
{
	const auto *g =
			reg.valid(id) ? reg.try_get<const comp::FromWeapon>(id) : nullptr;
	return g != nullptr ? g->spec : nullptr;
}

Spawned Battle::spawn(Layer layer, comp::Position pos, comp::Motion motion,
		comp::Physique physique, Borrowed<const CollisionMask> collider,
		comp::Allegiance allegiance)
{
	// Seeded with the spawning mask, not left null -- a mid-pipeline spawn has
	// no CapturePrior pass to fill this in, and a null mask misreads as
	// "turned", letting overlap-repair assign it onto a live element.
	const comp::PriorSilhouette prior{collider, pos.facing};

	Spawned s = make(layer);
	s.with(pos)
			.with(motion)
			.with(physique)
			.with(allegiance)
			.with(prior)
			.with(comp::CollisionScratch{})
			.with(comp::Appearing{});
	if (collider != nullptr)
		s.with(comp::Collider{collider});

	recordIfLanded(s.id(), allegiance);
	return s;
}

Spawned Battle::spawnBeam(
		Layer layer, comp::Beam beam, comp::Allegiance allegiance)
{
	// A beam is never solid and never moves, so Motion/Physique/
	// PriorSilhouette/CollisionScratch/Appearing are all dead weight --
	// resolveAgainst gates on collidable(testId) before touching any of them.
	Spawned s = make(layer);
	s.with(beam).with(allegiance);
	recordIfLanded(s.id(), allegiance);
	return s;
}

Spawned Battle::spawnEffect(
		Layer layer, comp::Position pos, comp::Allegiance allegiance)
{
	// A decorative particle: never solid, Position set once at spawn and
	// never touched again, so Motion/Physique/PriorSilhouette/
	// CollisionScratch/Collider/Appearing are all dead weight.
	Spawned s = make(layer);
	s.with(pos).with(allegiance);

	// Never a Beam, never a Warhead: the flavor is always Unknown for an
	// effect, which is correct -- nothing reads it for one.
	recordIfLanded(s.id(), allegiance);
	return s;
}

Spawned Battle::spawnEffect(Layer layer, comp::Position pos,
		comp::Motion motion, comp::Allegiance allegiance)
{
	// The one decoration that actually drifts (the explosion's debris):
	// spawnEffect's shape plus Motion, so Integrate still advances it.
	Spawned s = spawnEffect(layer, pos, allegiance);
	s.with(motion);
	return s;
}

// "BAD NEWS": an APPEARING element wedged inside something on spawn dies
// on the spot (process.c:427-449) -- full damage, then DISAPPEARING with
// its death response run now (hit_points is crew for a ship,
// element.h:126-133).
void Battle::killOverlapSpawn(EntityId id)
{
	// Reached only from resolveAgainst, which has already established that
	// both its ids are live; doDamage marks and drains, it never retires an
	// entity (see resolveAgainst for why nothing dies mid-Collide).
	assert(alive(id) && "killOverlapSpawn is given a live entity");

	const comp::ShipState *s = ship(id);
	const comp::Vitality *v = reg.try_get<comp::Vitality>(id);
	doDamage(*this, id,
			s != nullptr           ? s->crew
					: v != nullptr ? v->hitPoints
								   : 0);
	reg.get<comp::CollisionScratch>(id).collided = true;
	reg.emplace_or_replace<comp::Doomed>(id);
	runDeathResponses(id);
}

// Two death mechanisms, mutually exclusive per entity: the asteroid field's
// own cycle, or a SweepsOwnedOnDeath sweep of a dying ship's ordnance -- so
// which runs first is never observable. Each names a mechanic; neither is a
// hook the entity carries the code for.
void Battle::runDeathResponses(EntityId id) noexcept
{
	if (reg.all_of<comp::Asteroid>(id))
		advanceAsteroidCycle(*this, id);
	if (reg.all_of<comp::SweepsOwnedOnDeath>(id))
		sweepDeadShipOrdnance(*this, id);
}

// One candidate pair, from first possibility to full resolution -- the body
// of the C's ProcessCollisions loop (process.c:382-621). Returns whether the
// scanner is done for this walk (the C's mid-loop `return COLLISION`).
bool Battle::resolveAgainst(EntityId elemId, usize elemIdx, EntityId testId,
		usize testIdx, TimeValue maxTime)
{
	// Both ids stay live for this whole call, including the recursive descent
	// below: nothing is destroyed mid-Collide (removeElement's one caller is
	// the reap, a later sync point). Doomed marks, but does not retire.
	assert(alive(elemId) && alive(testId)
			&& "resolveAgainst is given two live entities");

	// Solidity first, before Motion/Physique/CollisionScratch fetch: elemId
	// is already collidable (collidePass's invariant), but testId may not be
	// -- a beam has no Collider and none of the rest either.
	if (!collidable(testId))
		return false;

	// Held for the rest of this call. A response can build an entity now
	// (the blast, the overlap-kill's rubble), but entt pages component
	// storage for pointer stability on insertion; only a destroy moves an
	// element, and nothing is destroyed mid-Collide -- removeElement's one
	// caller is the reap, a later sync point.
	comp::CollisionScratch &eScratch = reg.get<comp::CollisionScratch>(elemId);
	comp::CollisionScratch &tScratch = reg.get<comp::CollisionScratch>(testId);
	auto [ePos, eMotion, ePhys] =
			reg.try_get<comp::Position, comp::Motion, comp::Physique>(elemId);
	auto [tPos, tMotion, tPhys] =
			reg.try_get<comp::Position, comp::Motion, comp::Physique>(testId);

	if (!collisionPossible(reg.get<comp::Allegiance>(testId).owner, *tPhys,
				reg.get<comp::Allegiance>(elemId).owner, *ePhys,
				tScratch.collided, eScratch.collided,
				reg.all_of<comp::IgnoreSimilar>(testId),
				reg.all_of<comp::IgnoreSimilar>(elemId)))
		return false;

	// A transient element doesn't collide on its spawn frame
	// (process.c:389-394) -- so a missile can't detonate on its own muzzle.
	// More than one frame left, so one-frame PD fire still lands. The C's
	// FINITE_LIFE guard around this was implied by the test itself and is
	// gone (review-010 W3).
	const auto newbornTransient = [this](EntityId id) {
		return reg.all_of<comp::Appearing>(id) && isTransient(*this, id)
				&& framesLeft(*this, id) > 1;
	};
	if (newbornTransient(elemId) || newbornTransient(testId))
		return false;

	const bool bothSolid =
			!(isTransient(*this, elemId) || isTransient(*this, testId));
	Impact hit = sweptIntersect(bodyOf(*ePos, maskOf(reg, elemId)),
			bodyOf(*tPos, maskOf(reg, testId)), maxTime);

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
			const Body still{maskOf(reg, testId), worldToDisplay(tPos->next),
					worldToDisplay(tPos->next)};
			hit = sweptIntersect(bodyOf(*ePos, maskOf(reg, elemId)), still, 1);
			if (hit.time != 1)
				break;
		}

		const comp::PriorSilhouette &ePrior =
				reg.get<comp::PriorSilhouette>(elemId);
		const comp::PriorSilhouette &tPrior =
				reg.get<comp::PriorSilhouette>(testId);
		const bool eTurned = maskOf(reg, elemId) != ePrior.mask;
		const bool tTurned = maskOf(reg, testId) != tPrior.mask;
		if (!eTurned && !tTurned)
		{
			// Neither silhouette changed: either a spawn wedged inside
			// something (dies on the spot), or the tail of an already-resolved
			// contact, skipped so the original impulse can carry the pair apart
			// (process.c:427-451, 509-515).
			if (reg.all_of<comp::Appearing>(testId))
				killOverlapSpawn(testId);
			if (reg.all_of<comp::Appearing>(elemId))
			{
				killOverlapSpawn(elemId);
				return true;
			}
			hit = Impact{};
			break;
		}

		// A silhouette changed -- something rotated into a wall. Undo the turn
		// (process.c:453-506) and ask again. A null prior reverts to no
		// Collider, not a null mask -- collidable() reads Collider's presence,
		// not its mask.
		if (eTurned)
		{
			if (ePrior.mask != nullptr)
				reg.get<comp::Collider>(elemId).mask = ePrior.mask;
			else
				reg.remove<comp::Collider>(elemId);
			ePos->facing = ePrior.facing;
		}
		if (tTurned)
		{
			if (tPrior.mask != nullptr)
				reg.get<comp::Collider>(testId).mask = tPrior.mask;
			else
				reg.remove<comp::Collider>(testId);
			tPos->facing = tPrior.facing;
		}
		hit = sweptIntersect(bodyOf(*ePos, maskOf(reg, elemId)),
				bodyOf(*tPos, maskOf(reg, testId)), maxTime);
	}

	if (!hit)
		return false;

	const Vec2i elemStop = rewindTo(ePos->current, ePos->next, hit.time);
	const Vec2i testStop = rewindTo(tPos->current, tPos->next, hit.time);

	// Earliest-collision-wins (process.c:531-540): before resolving at
	// `hit.time`, recursively resolve whether either side hits something
	// earlier first; a yes on either side abandons this pair. Scanner's side
	// first.
	if (hit.time != 1)
	{
		const auto earlier = static_cast<TimeValue>(hit.time - 1);

		if (!eScratch.collided
				&& processCollisions(elemId, elemIdx, testIdx + 1, earlier))
			return false;
		std::tie(ePos, eMotion, ePhys) =
				reg.try_get<comp::Position, comp::Motion, comp::Physique>(
						elemId);
		std::tie(tPos, tMotion, tPhys) =
				reg.try_get<comp::Position, comp::Motion, comp::Physique>(
						testId);

		if (!tScratch.collided)
		{
			// The C scans the test element's earlier candidates from the
			// scanner's successor -- or from the head when the test element
			// is newly spawned (process.c:535-540).
			const usize from =
					reg.all_of<comp::Appearing>(testId) ? 0 : elemIdx + 1;
			if (processCollisions(testId, testIdx, from, earlier))
				return false;
		}
		std::tie(ePos, eMotion, ePhys) =
				reg.try_get<comp::Position, comp::Motion, comp::Physique>(
						elemId);
		std::tie(tPos, tMotion, tPhys) =
				reg.try_get<comp::Position, comp::Motion, comp::Physique>(
						testId);
	}

	// Resolution. The response decides who stops -- each raises Collided on
	// itself, exactly as the C's collision_funcs raise COLLISION -- and runs
	// ship-side first when the TEST element is the ship (process.c:549-570).
	const bool elemHad = eScratch.collided;
	const bool testHad = tScratch.collided;
	const bool bothSolidNow =
			!(isTransient(*this, elemId) || isTransient(*this, testId));

	CollisionEvent event;
	event.a.id = elemId;
	event.b.id = testId;
	event.at = elemStop;
	event.a.before = worldVelocityOf(*eMotion);
	event.b.before = worldVelocityOf(*tMotion);

	// Dispatch keyed on Warhead's presence: has<Warhead> is a shot, everything
	// else reaching here is solid (ship/asteroid/planet). Both sides' dispatch
	// is decided before either runs.
	const bool eIsWeapon = reg.all_of<comp::Warhead>(elemId);
	const bool tIsWeapon = reg.all_of<comp::Warhead>(testId);
	const auto respond = [this](bool isWeapon, EntityId id, EntityId otherId) {
		if (isWeapon)
			weaponCollision(*this, id, otherId);
		else
			solidCollision(*this, id, otherId);
	};
	if (reg.all_of<comp::ShipState>(testId))
	{
		respond(tIsWeapon, testId, elemId);
		respond(eIsWeapon, elemId, testId);
	}
	else
	{
		respond(eIsWeapon, elemId, testId);
		respond(tIsWeapon, testId, elemId);
	}

	std::tie(ePos, eMotion, ePhys) =
			reg.try_get<comp::Position, comp::Motion, comp::Physique>(elemId);
	std::tie(tPos, tMotion, tPhys) =
			reg.try_get<comp::Position, comp::Motion, comp::Physique>(testId);

	// Whoever NEWLY raised Collided stops at the impact point
	// (process.c:572-596); a side that was already stopped keeps the
	// position its first collision gave it.
	if (tScratch.collided && !testHad)
		tPos->next = testStop;

	bool impulsed = false;
	if (eScratch.collided && !elemHad)
	{
		ePos->next = elemStop;

		// Momentum exchange is solid-on-solid only (process.c:598-601). A
		// weapon hit is resolved by damage, in the dispatch above.
		if (bothSolidNow)
		{
			// Pure physics is told who is a ship by a ShipState pointer, null
			// for anything else.
			applyImpulse(*ePos, *eMotion, *ePhys, ship(elemId), eScratch, *tPos,
					*tMotion, *tPhys, ship(testId), tScratch);
			impulsed = true;

			// collide.c:104-110: an impulse invalidates the at-max bookkeeping.
			if (reg.all_of<comp::ShipState>(elemId))
				if (comp::ShipState *ss = ship(elemId))
					ss->speed = SpeedState::Normal;
			if (reg.all_of<comp::ShipState>(testId))
				if (comp::ShipState *ss = ship(testId))
					ss->speed = SpeedState::Normal;
		}
	}

	event.a.after = worldVelocityOf(*eMotion);
	event.b.after = worldVelocityOf(*tMotion);
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
	if (eScratch.collided)
		return true;
	if (!collidable(elemId))
	{
		eScratch.collided = true;
		return true;
	}
	return false;
}

// ProcessCollisions (process.c:361-627): walks candidates from `fromIdx` in
// collideOrder_. Returns whether `elem` ended the walk stopped. Nothing is
// destroyed mid-Collide, so an index into collideOrder_ stays valid.
bool Battle::processCollisions(
		EntityId elemId, usize elemIdx, usize fromIdx, TimeValue maxTime)
{
	// Every id in the snapshot is still live: it was taken this pass and
	// nothing is destroyed mid-Collide (see resolveAgainst).
	assert(alive(elemId) && "processCollisions scans for a live entity");

	for (usize idx = fromIdx; idx < collideOrder_.size();)
	{
		const EntityId testId = collideOrder_[idx];
		assert(alive(testId) && "the collide snapshot outlived an entity");

		const usize succIdx = idx + 1;

		if (!(testId == elemId)
				&& resolveAgainst(elemId, elemIdx, testId, idx, maxTime))
			return true;

		idx = succIdx;
	}

	return reg.get<comp::CollisionScratch>(elemId).collided;
}

// CapturePrior (pipeline slot 1): the silhouette/facing every element enters
// the frame with, batched at frame start -- nothing before Collide (slot 9)
// can change a mask or a facing anyway. Collided is cleared here too: it
// must read false before Collide sets it fresh this frame.
void Battle::capturePriorPass() noexcept
{
	for (auto [id, order, pos, prior, scratch] :
			reg.view<comp::Order, comp::Position, comp::PriorSilhouette,
					   comp::CollisionScratch>()
					.each())
	{
		prior.mask = maskOf(reg, id);
		prior.facing = pos.facing;
		scratch.collided = false;
	}
}

// AgeAndReap-mark (pipeline slot 2): death check at frame start, before
// anything else touches the element (misc.c:210,221). The matching
// decrement is separate (ageDecrementPass). eachOrdered, not a bare
// view<Lifetime> -- death responses draw RNG, so discovery order is
// gameplay, not incidental.
void Battle::ageAndReapMarkPass()
{
	eachOrdered([this](EntityId id) {
		const comp::Lifetime *life = reg.try_get<comp::Lifetime>(id);
		if (life == nullptr || life->remaining != 0)
			return;
		reg.emplace<comp::Doomed>(id);
		runDeathResponses(id);
	});
}

// Animate (pipeline slot 7): the asteroid's tumble (Spin) and the flame's
// frame-advance (AnimFrame), each its own typed sub-pass since neither
// reads or writes outside its own entity. Appearing suppresses both for
// one frame (a weapon's animation starts on its second frame alive).
void Battle::animatePass()
{
	// asteroid_preprocess (misc.c:107-128): tumbles by Spin; the C's rotation
	// lives in the sprite frame, here in `facing`. eachOrdered, not a bare
	// view -- order is load-bearing (sim_test.cpp's testStepVisitsInListOrder).
	eachOrdered<comp::Position, comp::Spin>(entt::exclude<comp::Appearing>,
			[](EntityId, comp::Position &pos, comp::Spin &spin) {
				if (spin.countdown > 0)
				{
					--spin.countdown;
					return;
				}
				pos.facing += spin.backwards ? -1 : 1;
				spin.countdown = spin.period;
			});

	// flame_preprocess (ilwrath.c:126-139): frame advances every frame it
	// lives, and the collision silhouette follows -- why the flame GROWS as it
	// flies (process.c:159-160). Collider via try_get: the linger frame may
	// have none.
	reg.view<comp::Order, comp::AnimFrame, comp::FrameDriven>(
			   entt::exclude<comp::Appearing>)
			.each([this](EntityId id, comp::Order &, comp::AnimFrame &frame) {
				++frame.n;
				Borrowed<const WeaponSpec> ws = weaponSpec(id);
				if (ws != nullptr && !ws->masks.empty())
				{
					if (comp::Collider *c = reg.try_get<comp::Collider>(id))
						c->mask = &ws->masks[static_cast<usize>(frame.n)
								% ws->masks.size()];
				}
			});
}

// Integrate (pipeline slot 8): SetUpElement's seeding (process.c:117-126)
// plus the motion add (process.c:163,172-173), batched over the whole spine
// instead of interleaved per entity. The wrap still happens at Commit
// (slot 12), not here -- design-notes D4.
void Battle::integratePass() noexcept
{
	// A beam has no Position, so this view never sees one.
	for (auto [id, order, pos, mot] :
			reg.view<comp::Order, comp::Position, comp::Motion>().each())
	{
		if (reg.all_of<comp::Appearing>(id))
			pos.next = pos.current;

		pos.next += mot.velocity.advance(1);
	}
}

// Collide (pipeline slot 9): the pair-walk (processCollisions/resolveAgainst)
// over the fully-integrated spine. collideOrder_ is a snapshot sorted by
// (Order.layer, Order.seq), not a view: the walk re-enters at arbitrary
// indices (testIdx+1, elemIdx+1, or 0 for Appearing) that a forward-only
// iterator can't serve.
void Battle::collidePass()
{
	ensureOrdered();
	collideOrder_.clear();
	for (EntityId const id : reg.view<comp::Order>())
		collideOrder_.push_back(id);

	for (usize i = 0; i < collideOrder_.size(); ++i)
	{
		const EntityId id = collideOrder_[i];
		if (collidable(id) && !reg.get<comp::CollisionScratch>(id).collided)
		{
			// Successors only, so each pair is visited once per frame.
			(void)processCollisions(id, i, i + 1, kMaxTimeValue);
		}
	}
}

// Sync point, 11a: one summed deltaCrew and one death check per victim --
// applied once here instead of on whichever hit landed first.
void Battle::applyDamageIncoming() noexcept
{
	reg.view<comp::DamageIncoming, comp::ShipState>().each(
			[this](EntityId id, comp::DamageIncoming &di, comp::ShipState &s) {
				if (!deltaCrew(s, -di.amount))
					startShipExplosion(*this, id);
			});
	reg.clear<comp::DamageIncoming>();
}

// AgeDecrement: runs between Integrate and Collide, not batched at a sync
// point. Too early double-counts a ship's own warp-in Lifetime (attached by
// ShipMachines on the appearing frame). Too late lets a same-frame kill
// (Lifetime{0} without Doomed) decrement past zero -- and remaining must
// land on exactly 0, or the death is never detected next frame.
void Battle::ageDecrementPass() noexcept
{
	reg.view<comp::Order, comp::Lifetime>(entt::exclude<comp::Doomed>)
			.each([](comp::Order &, comp::Lifetime &life) {
				--life.remaining;
			});
}

// Sync point, 11c: destroy every element already Doomed -- marked in slot 2
// or mid-Collide (a weapon's self-spend, or overlap-repair's overlap-kill).
// Its death response already ran when Doomed was set; nothing runs again.
// view<Doomed> walks pool order, not (layer, seq) -- fine, since nothing
// here reads order and destruction never touches a survivor's Order stamp.
void Battle::reapPass() noexcept
{
	// Erasing the entity currently returned by a view's iterator is safe by
	// entt's own contract, so removing it mid-walk needs no extra care.
	for (EntityId const id : reg.view<comp::Doomed>())
		removeElement(id);
}

// Sync point, 11e: runs BEFORE landPendingSpawns puts this frame's spawns
// into the walk -- a newborn keeps Appearing through its own first live
// frame, and joining Order is what keeps that frame from being stripped
// one early. CollisionScratch's presence is the other filter (a beam has
// none).
void Battle::flagsEndOfFramePass() noexcept
{
	// A frame without a collision ends DefyPhysics (process.c:824-827): it
	// has to expire, or the first stationary contact disables the collision
	// stagger for good.
	reg.view<comp::Order, comp::CollisionScratch>().each(
			[](comp::Order &, comp::CollisionScratch &scratch) {
				if (!scratch.collided)
					scratch.defyPhysics = false;
			});

	// Only what is already in the walk: this frame's spawns have not landed
	// yet, and clearing theirs here would strip the first live frame they
	// are entitled to.
	for (const EntityId id : reg.view<comp::Order, comp::Appearing>())
		reg.erase<comp::Appearing>(id);
}

// Sync point, 11d: put this frame's spawns into the walk, in emission
// order. The entities already exist and are fully built -- only the Order
// was withheld, which is what kept them out of every pass this frame.
// Runs after flagsEndOfFramePass so a fresh spawn's Appearing survives to
// its first real frame.
//
// Nothing may touch the Order pool between the top of step() and here: see
// ensureOrdered, and docs/worknotes.md for why a tag on the entity is not
// an alternative.
void Battle::landPendingSpawns()
{
	// Cleared first: a deferred construction runs with spawns landing, not
	// deferring, so it takes its Order here in queue order like the rest.
	deferSpawns_ = false;

	for (const PendingSpawn &p : pending_)
	{
		if (p.deferred != nullptr)
		{
			// Its RNG draws happen here, in queue order -- not at emission,
			// which would put them out of step with this sync point's other
			// draws.
			p.deferred(*this, p.deferredMask);
			continue;
		}

		reg.emplace<comp::Order>(p.id, nextOrder(p.layer));
		recordSpawn(p.id, reg.get<comp::Allegiance>(p.id));
	}
	pending_.clear();
}

// Commit (pipeline slot 12): the wrap and the publish, unchanged from
// today's postProcess commit (process.c:899-916). A beam has no Position,
// so this view never sees one.
void Battle::commitPass() noexcept
{
	for (auto [id, order, pos] : reg.view<comp::Order, comp::Position>().each())
	{
		pos.next = wrap(pos.next);
		pos.current = pos.next;
	}
}

void Battle::step()
{
	collisions_.clear();
	spawns_.clear();

	// From here to the sync point every spawn is built but held out of the
	// walk; landPendingSpawns clears this and puts them in.
	deferSpawns_ = true;

	capturePriorPass();
	ageAndReapMarkPass();
	energyRegenPass(*this);
	// Ahead of ShipMachines, which is where an arriving ship loses WarpingIn:
	// the exclusion these mechanics gate on has to still be attached when the
	// query tests it (Specials.hpp).
	preTurnSpecialsPass(*this);
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
	landPendingSpawns();

	commitPass();

	++frame_;
}

}  // namespace uqm::sim
