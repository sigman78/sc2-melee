// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Battle.hpp"

#include "engine/core/Types.hpp"
#include "sim/Damage.hpp"
#include "sim/Gravity.hpp"
#include "sim/Impulse.hpp"
#include "sim/ShipSystems.hpp"
#include "sim/World.hpp"

#include <cassert>
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
[[nodiscard]] bool
collisionPossible(const Element &test, const Element &elem) noexcept
{
	if (!test.collidable())
		return false;
	if (any(test.flags & elem.flags & ElementFlags::Collided))
		return false;
	if (any(test.flags & elem.flags & ElementFlags::IgnoreSimilar)
			&& test.owner != kNoEntity && test.owner == elem.owner)
		return false;
	if (test.mass == 0 && elem.mass == 0)
		return false;
	return true;
}

// Masks are display pixels, positions are world units -- the conversion
// isn't optional, or the intersect test sees everything 4x further apart.
// The C converts at this exact boundary (collide.h:44-54).
[[nodiscard]] Body
bodyOf(const Element &e) noexcept
{
	return Body{e.mask, worldToDisplay(e.current), worldToDisplay(e.next)};
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
worldVelocityOf(const Element &e) noexcept
{
	const Vec2i v = e.velocity.current();
	return Vec2i{velocityToWorld(v.x), velocityToWorld(v.y)};
}

}  // namespace

Battle::Battle(u32 seed) : rng_(seed)
{
	// One chunk was the whole battle in the old arena (EntityList's
	// kChunkSize); the reserve keeps the steady-state step allocation-free.
	reg_.storage<Element>().reserve(64);
	reg_.storage<OrderLink>().reserve(64);
	reg_.storage<PriorSilhouette>().reserve(64);
}

EntityId
Battle::next(EntityId id) const noexcept
{
	assert(alive(id) && "next() of a dead entity");
	return reg_.get<OrderLink>(id).next;
}

EntityId
Battle::prev(EntityId id) const noexcept
{
	assert(alive(id) && "prev() of a dead entity");
	return reg_.get<OrderLink>(id).prev;
}

void
Battle::linkAtLayerTail(Layer layer, EntityId id) noexcept
{
	// The anchor is this layer's tail, or the nearest earlier layer's --
	// an empty layer occupies no space in the chain, only a position.
	EntityId after = kNoEntity;
	for (int l = static_cast<int>(layer); l >= 0 && after == kNoEntity; --l)
		after = layerTail_[static_cast<usize>(l)];

	OrderLink &s = reg_.get<OrderLink>(id);
	s.layer = layer;
	if (after == kNoEntity)
	{
		s.prev = kNoEntity;
		s.next = head_;
		if (head_ != kNoEntity)
			reg_.get<OrderLink>(head_).prev = id;
		head_ = id;
		if (tail_ == kNoEntity)
			tail_ = id;
	}
	else
	{
		OrderLink &prevLink = reg_.get<OrderLink>(after);
		s.prev = after;
		s.next = prevLink.next;
		if (prevLink.next != kNoEntity)
			reg_.get<OrderLink>(prevLink.next).prev = id;
		else
			tail_ = id;
		prevLink.next = id;
	}
	layerTail_[static_cast<usize>(layer)] = id;
}

void
Battle::removeElement(EntityId id) noexcept
{
	if (!alive(id))
		return;

	const OrderLink s = reg_.get<OrderLink>(id);
	if (s.prev != kNoEntity)
		reg_.get<OrderLink>(s.prev).next = s.next;
	else
		head_ = s.next;
	if (s.next != kNoEntity)
		reg_.get<OrderLink>(s.next).prev = s.prev;
	else
		tail_ = s.prev;

	// A layer's tail retreats to the predecessor only if that predecessor
	// is in the same layer; otherwise the layer just became empty.
	if (layerTail_[static_cast<usize>(s.layer)] == id)
	{
		const bool prevSameLayer = s.prev != kNoEntity
				&& reg_.get<OrderLink>(s.prev).layer == s.layer;
		layerTail_[static_cast<usize>(s.layer)] =
				prevSameLayer ? s.prev : kNoEntity;
	}

	// Bumps the entity's version, which is what turns a surviving handle
	// into a detectable mistake instead of a read of the next tenant.
	reg_.destroy(id);
	--count_;
}

void
Battle::recordSpawn(EntityId id, const Element &e)
{
	spawns_.push_back(SpawnEvent{id, e.kind, e.playerNr});
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
Battle::spawn(Layer layer, Element e)
{
	e.flags |= ElementFlags::Appearing;
	const EntityId id = reg_.create();
	recordSpawn(id, e);

	// Seeded from the element being spawned, not left null: a mid-pipeline
	// spawn (a death hook's replacement asteroid) reaches Collide with no
	// CapturePrior pass of its own to have filled this in, and a null mask
	// reads as "turned" against anything it overlaps, letting the
	// overlap-repair protocol assign that null mask onto a live element.
	const PriorSilhouette prior{e.mask, e.facing};
	reg_.emplace<Element>(id, std::move(e));
	reg_.emplace<OrderLink>(id);
	reg_.emplace<PriorSilhouette>(id, prior);
	reg_.emplace<Seq>(id, Seq{nextSeq_++});
	linkAtLayerTail(layer, id);
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
	doDamage(*this, id, s != nullptr ? s->crew : e->hitPoints);
	e = get(id);
	if (e == nullptr)
		return;
	e->flags |= ElementFlags::Collided | ElementFlags::Disappearing;
	if (e->onDeath != nullptr)
		e->onDeath(*this, id);
}

// One candidate pair, from first possibility to full resolution -- the body
// of the C's ProcessCollisions loop (process.c:382-621). Returns whether the
// scanner is done for this walk (the C's mid-loop `return COLLISION`).
bool
Battle::resolveAgainst(
		EntityId elemId, EntityId testId, EntityId succ, TimeValue maxTime)
{
	auto e = get(elemId);
	if (e == nullptr)
		return true;
	auto t = get(testId);
	if (t == nullptr || !collisionPossible(*t, *e))
		return false;

	// A transient element doesn't collide on its spawn frame (process.c:389-394)
	// -- exempts FINITE_LIFE-with-Appearing on EITHER side, so a missile can't
	// detonate on its own muzzle; lifeSpan > 1 still lets one-frame PD fire.
	if (any((e->flags | t->flags) & ElementFlags::FiniteLife)
			&& ((any(e->flags & ElementFlags::Appearing) && e->lifeSpan > 1)
					|| (any(t->flags & ElementFlags::Appearing)
							&& t->lifeSpan > 1)))
		return false;

	const bool bothSolid =
			!any((e->flags | t->flags) & ElementFlags::FiniteLife);
	Impact hit = sweptIntersect(bodyOf(*e), bodyOf(*t), maxTime);

	// "BAD NEWS" (process.c:397-516): impact at time 1 between two solids is a
	// standing overlap, not a new collision -- a repair protocol, not a
	// response, or responding again welds ships together. Weapons are exempt.
	while (hit.time == 1 && bothSolid)
	{
		if (any(e->flags & ElementFlags::Collided))
		{
			// The scanner already stopped this frame; the overlap is real
			// only if it persists with the test element taken at its END
			// position (process.c:405-413).
			const Body still{
					t->mask, worldToDisplay(t->next), worldToDisplay(t->next)};
			hit = sweptIntersect(bodyOf(*e), still, 1);
			if (hit.time != 1)
				break;
		}

		const PriorSilhouette &ePrior = reg_.get<PriorSilhouette>(elemId);
		const PriorSilhouette &tPrior = reg_.get<PriorSilhouette>(testId);
		const bool eTurned = e->mask != ePrior.mask;
		const bool tTurned = t->mask != tPrior.mask;
		if (!eTurned && !tTurned)
		{
			// Neither silhouette changed: either a spawn wedged inside something
			// (dies on the spot), or the tail of an already-resolved contact, skipped
			// so the original impulse can carry the pair apart (process.c:427-451, 509-515).
			if (any(t->flags & ElementFlags::Appearing))
				killOverlapSpawn(testId);
			if (any(e->flags & ElementFlags::Appearing))
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
		if (eTurned)
		{
			e->mask = ePrior.mask;
			e->facing = ePrior.facing;
		}
		if (tTurned)
		{
			t->mask = tPrior.mask;
			t->facing = tPrior.facing;
		}
		hit = sweptIntersect(bodyOf(*e), bodyOf(*t), maxTime);
	}

	if (!hit)
		return false;

	const Vec2i elemStop = rewindTo(e->current, e->next, hit.time);
	const Vec2i testStop = rewindTo(t->current, t->next, hit.time);

	// Earliest-collision-wins (process.c:531-540): before resolving at
	// `hit.time`, recursively resolve whether either side hits something
	// earlier first; a yes on either side abandons this pair. Scanner's side first.
	if (hit.time != 1)
	{
		const auto earlier = static_cast<TimeValue>(hit.time - 1);

		if (!any(e->flags & ElementFlags::Collided)
				&& processCollisions(elemId, succ, earlier))
			return false;
		e = get(elemId);
		t = get(testId);
		if (e == nullptr)
			return true;
		if (t == nullptr)
			return false;

		if (!any(t->flags & ElementFlags::Collided))
		{
			// The C scans the test element's earlier candidates from the
			// scanner's successor -- or from the head when the test element
			// is newly spawned (process.c:535-540).
			const EntityId from = any(t->flags & ElementFlags::Appearing)
					? front()
					: next(elemId);
			if (processCollisions(testId, from, earlier))
				return false;
		}
		e = get(elemId);
		t = get(testId);
		if (e == nullptr)
			return true;
		if (t == nullptr)
			return false;
	}

	// Resolution. The hooks decide who stops -- each raises Collided on
	// itself, exactly as the C's collision_funcs raise COLLISION -- and run
	// ship-side first when the TEST element is the ship (process.c:549-570).
	const bool elemHad = any(e->flags & ElementFlags::Collided);
	const bool testHad = any(t->flags & ElementFlags::Collided);
	const bool bothSolidNow =
			!any((e->flags | t->flags) & ElementFlags::FiniteLife);

	e->collidedWith = testId;
	t->collidedWith = elemId;

	CollisionEvent event;
	event.a = elemId;
	event.b = testId;
	event.at = elemStop;
	event.beforeA = worldVelocityOf(*e);
	event.beforeB = worldVelocityOf(*t);

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

	// Whoever NEWLY raised Collided stops at the impact point
	// (process.c:572-596); a side that was already stopped keeps the
	// position its first collision gave it.
	if (t != nullptr && any(t->flags & ElementFlags::Collided) && !testHad)
		t->next = testStop;

	bool impulsed = false;
	if (e != nullptr && any(e->flags & ElementFlags::Collided) && !elemHad)
	{
		e->next = elemStop;

		// Momentum exchange is solid-on-solid only (process.c:598-601). A
		// weapon hit is resolved by damage, from the collidedWith set above.
		if (t != nullptr && bothSolidNow)
		{
			// The trait left the struct, so pure physics is told who is a
			// ship instead of reading a flag (review-004 X4's friction note).
			applyImpulse(*e, reg_.all_of<PlayerShip>(elemId), *t,
					reg_.all_of<PlayerShip>(testId));
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

	event.afterA = e != nullptr ? worldVelocityOf(*e) : event.beforeA;
	event.afterB = t != nullptr ? worldVelocityOf(*t) : event.beforeB;
	collisions_.push_back(event);

	if (impulsed)
	{
		// Both participants immediately re-scan the whole list
		// (process.c:603-606): a pile-up chains within this frame instead of
		// resolving one pair per step.
		processCollisions(elemId, front(), kMaxTimeValue);
		processCollisions(testId, front(), kMaxTimeValue);
	}

	// Keeps scanning unless out of the game for the frame (process.c:609-618):
	// stopped, or no longer collidable -- a ship merely hit by a missile is
	// neither, and can still bounce off another ship this frame.
	e = get(elemId);
	if (e == nullptr || any(e->flags & ElementFlags::Collided))
		return true;
	if (!e->collidable())
	{
		e->flags |= ElementFlags::Collided;
		return true;
	}
	return false;
}

// ProcessCollisions (process.c:361-627): walks candidates from `first`.
// Returns whether `elem` ended the walk stopped. Z4 drops the straggler
// preprocessing the C interleaved here (process.c:371-373): Integrate
// (pipeline slot 8) already ran over the whole spine before Collide ever
// starts, so there is nothing left unintegrated for this walk to catch up.
bool
Battle::processCollisions(EntityId elemId, EntityId first, TimeValue maxTime)
{
	for (EntityId testId = first; testId != kNoEntity;)
	{
		if (get(testId) == nullptr)
			break;

		const EntityId succ = next(testId);

		if (!(testId == elemId) && resolveAgainst(elemId, testId, succ, maxTime))
			return true;

		testId = succ;
	}

	auto e = get(elemId);
	return e == nullptr || any(e->flags & ElementFlags::Collided);
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
	for (EntityId id = front(); id != kNoEntity; id = next(id))
	{
		auto e = get(id);
		if (e == nullptr)
			continue;
		PriorSilhouette &prior = reg_.get<PriorSilhouette>(id);
		prior.mask = e->mask;
		prior.facing = e->facing;
		e->flags &= ~ElementFlags::Collided;
	}
}

// AgeAndReap-mark (pipeline slot 2): the death check stays exactly where it
// was -- frame start, before anything else touches the element (process.c
// has no FINITE_LIFE guard: do_damage kills a non-aging asteroid by
// assigning life_span = 0 directly, misc.c:210,221). Only the CHECK moved
// here; the matching decrement is slot 11b, at the sync point.
void
Battle::ageAndReapMarkPass()
{
	for (EntityId id = front(); id != kNoEntity;)
	{
		const EntityId nextId = next(id);
		auto e = get(id);
		if (e != nullptr && e->lifeSpan == 0)
		{
			e->flags |= ElementFlags::Disappearing;
			if (e->onDeath != nullptr)
				e->onDeath(*this, id);
		}
		id = nextId;
	}
}

// Animate (pipeline slot 7): what is left of the per-element preProcess hook
// dispatch once ships (ShipMachines/Turn/Thrust, ShipSystems.cpp) and Guided
// shots (GuidedSteer) have their own passes -- the flame's frame-advance,
// the asteroid's tumble. Appearing still suppresses the hook for one frame,
// exactly as it always has (a weapon's hook does not run until its second
// frame alive).
void
Battle::animatePass()
{
	for (EntityId id = front(); id != kNoEntity;)
	{
		const EntityId nextId = next(id);
		auto e = get(id);
		if (e != nullptr && !reg_.all_of<PlayerShip>(id)
				&& !reg_.all_of<Guided>(id) && e->preProcess != nullptr
				&& !any(e->flags & ElementFlags::Appearing))
		{
			e->preProcess(*this, id);
		}
		id = nextId;
	}
}

// Integrate (pipeline slot 8): SetUpElement's seeding (process.c:117-126)
// plus the motion add (process.c:163,172-173), batched over the whole spine
// instead of interleaved per entity. The wrap still happens at Commit
// (slot 12), not here -- design-notes D4.
void
Battle::integratePass() noexcept
{
	for (EntityId id = front(); id != kNoEntity; id = next(id))
	{
		auto e = get(id);
		if (e == nullptr || reg_.all_of<IgnoreVelocity>(id))
			continue;

		// BeamGeometry is exempt: seeding next from current would collapse
		// the beam to a point (its two points are the beam, not motion).
		if (any(e->flags & ElementFlags::Appearing)
				&& !reg_.all_of<BeamGeometry>(id))
			e->next = e->current;

		const Vec2i delta = e->velocity.advance(1);
		e->next = Vec2i{e->next.x + delta.x, e->next.y + delta.y};
	}
}

// Collide (pipeline slot 9): the same pair-walk machinery as always
// (processCollisions/resolveAgainst), now running over a spine that is
// already fully integrated -- no straggler preprocessing needed, and no
// catch-up pass, because nothing spawned this frame exists yet (that is
// slot 11d). onCollision hooks still run inline; ship crew damage they
// cause now lands in DamageIncoming instead of applying on the spot (see
// Damage.cpp), applied at the sync point below.
void
Battle::collidePass()
{
	for (EntityId id = front(); id != kNoEntity; id = next(id))
	{
		auto e = get(id);
		if (e != nullptr && e->collidable()
				&& !any(e->flags & ElementFlags::Collided))
		{
			// Successors only, so each pair is visited once per frame.
			(void)processCollisions(id, next(id), kMaxTimeValue);
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
//   own warp-in: ShipMachines is what sets FiniteLife and seeds lifeSpan =
//   kWarpInFrames on the appearing frame, and that has to happen before a
//   decrement can apply to it, or the first traced frame is off by one
//   (kWarpInFrames instead of kWarpInFrames - 1, caught by replay_test's
//   --trace diff against the pre-Z4 baseline).
// - Too late (at the sync point, after Collide/Fire) lets a same-frame kill
//   that sets lifeSpan = 0 without Disappearing -- doDamage's non-ship
//   branch, used by a piercing pair where only one side's own
//   weaponCollision fall-through sets Disappearing, and by the PD special
//   killing a shot outright -- get decremented straight past zero to -1
//   this same frame. A lifeSpan that never lands back on exactly 0 is never
//   detected as dead next frame; testOpposingMissilesDestroyEachOther and
//   testPointDefenceBurnsOwnNuke both failed this way before landing here.
void
Battle::ageDecrementPass() noexcept
{
	for (EntityId id = front(); id != kNoEntity; id = next(id))
	{
		auto e = get(id);
		if (e != nullptr && any(e->flags & ElementFlags::FiniteLife)
				&& !any(e->flags & ElementFlags::Disappearing))
			--e->lifeSpan;
	}
}

// Sync point, 11c: destroy every element already Disappearing -- marked in
// slot 2 from a death detected this frame, or mid-Collide (slot 9) by a
// weapon's own self-spend or the overlap-repair protocol's overlap-kill.
// Either way its onDeath already ran at the point Disappearing was set;
// nothing runs again here, exactly as postProcessPass's reap branch never
// called postProcess either.
void
Battle::reapPass() noexcept
{
	for (EntityId id = front(); id != kNoEntity;)
	{
		const EntityId nextId = next(id);
		auto e = get(id);
		if (e != nullptr && any(e->flags & ElementFlags::Disappearing))
			removeElement(id);
		id = nextId;
	}
}

// Sync point, 11e: end-of-frame flag housekeeping over the spine as it
// stands BEFORE 11d creates this frame's spawns (run first here for
// exactly that reason -- see drainSpawnCommands). A newborn must keep
// Appearing through its own first live frame, which under Z4 is next
// frame's pipeline, not a same-frame catch-up; clearing it now would be one
// frame early.
void
Battle::flagsEndOfFramePass() noexcept
{
	for (EntityId id = front(); id != kNoEntity; id = next(id))
	{
		auto e = get(id);
		if (e == nullptr)
			continue;
		// A frame without a collision ends DefyPhysics (process.c:824-827):
		// it has to expire, or the first stationary contact disables the
		// collision stagger for good.
		if (!any(e->flags & ElementFlags::Collided))
			e->flags &= ~ElementFlags::DefyPhysics;
		e->flags &= ~ElementFlags::Appearing;
	}
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

		const EntityId id = spawn(cmd.layer, std::move(cmd.element));
		if (cmd.weaponSpec != nullptr)
			attachWeaponSpec(id, cmd.weaponSpec);
		if (cmd.guided)
			attach<Guided>(id, *cmd.guided);
		if (cmd.ignoreVelocity)
			attach<IgnoreVelocity>(id);
		if (cmd.beamGeometry)
			attach<BeamGeometry>(id);
	}
	spawnCommands_.clear();
}

// Commit (pipeline slot 12): the wrap and the publish, unchanged from
// today's postProcess commit (process.c:899-916) except that it now runs as
// its own whole-spine pass instead of once per entity inline with its hook.
void
Battle::commitPass() noexcept
{
	for (EntityId id = front(); id != kNoEntity; id = next(id))
	{
		auto e = get(id);
		if (e == nullptr || reg_.all_of<BeamGeometry>(id))
			continue;
		e->next = wrap(e->next);
		e->current = e->next;
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
