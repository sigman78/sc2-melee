// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Battle.hpp"

#include "sim/Damage.hpp"
#include "sim/Impulse.hpp"
#include "sim/World.hpp"

#include <cassert>
#include <utility>

namespace uqm::sim {

namespace {

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
	const std::int32_t t = static_cast<std::int32_t>(time) - 1;  // 0..256
	return Vec2i{from.x
				+ static_cast<std::int32_t>(
						(std::int64_t{to.x - from.x} * t) >> kTimeShift),
		from.y
				+ static_cast<std::int32_t>(
						(std::int64_t{to.y - from.y} * t) >> kTimeShift)};
}

// In world units per frame, so consumers never see the packed fixed point.
[[nodiscard]] Vec2i
worldVelocityOf(const Element &e) noexcept
{
	const Vec2i v = e.velocity.current();
	return Vec2i{velocityToWorld(v.x), velocityToWorld(v.y)};
}

}  // namespace

Battle::Battle(std::uint32_t seed) : rng_(seed)
{
	// One chunk was the whole battle in the old arena (EntityList's
	// kChunkSize); the reserve keeps the steady-state step allocation-free.
	reg_.storage<Element>().reserve(64);
	reg_.storage<OrderLink>().reserve(64);
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
Battle::linkAfter(EntityId after, EntityId id) noexcept
{
	OrderLink &s = reg_.get<OrderLink>(id);
	if (after == kNoEntity)
	{
		s.prev = kNoEntity;
		s.next = head_;
		if (head_ != kNoEntity)
			reg_.get<OrderLink>(head_).prev = id;
		head_ = id;
		if (tail_ == kNoEntity)
			tail_ = id;
		return;
	}

	OrderLink &prevLink = reg_.get<OrderLink>(after);
	s.prev = after;
	s.next = prevLink.next;
	if (prevLink.next != kNoEntity)
		reg_.get<OrderLink>(prevLink.next).prev = id;
	else
		tail_ = id;
	prevLink.next = id;
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
Battle::spawn(EntityId after, Element e)
{
	e.flags |= ElementFlags::Appearing;
	const EntityId id = reg_.create();
	recordSpawn(id, e);
	reg_.emplace<Element>(id, std::move(e));
	reg_.emplace<OrderLink>(id);
	linkAfter(after, id);
	++count_;
	return id;
}

EntityId
Battle::spawnFront(Element e)
{
	return spawn(kNoEntity, std::move(e));
}

EntityId
Battle::spawnBack(Element e)
{
	return spawn(tail_, std::move(e));
}

EntityId
Battle::insertAfter(EntityId after, Element e)
{
	assert((after == kNoEntity || alive(after)) && "insert after a dead entity");
	return spawn(after, std::move(e));
}

// PreProcess (process.c:128-186): a spawned element is seeded with next =
// current (process.c:117-126) but still moves -- gated on IGNORE_VELOCITY
// alone (process.c:163), not Appearing, which suppresses only the hook.

// Appearing is cleared here only for player ships (process.c:150-151, "want
// to preprocess ship"). A weapon keeps it through its first frame, so its
// hook does not run until the second.
void
Battle::preProcessOne(EntityId id) noexcept
{
	auto e = get(id);
	if (e == nullptr)
		return;

	// Death is `life_span == 0` and nothing else -- process.c:133 has no
	// FINITE_LIFE guard, so do_damage kills a non-aging asteroid by assigning
	// life_span = 0 (misc.c:210,221); a persistent element is born at 1, not 0.
	if (e->lifeSpan == 0)
	{
		e->flags |= ElementFlags::Disappearing;
		if (e->onDeath != nullptr)
		{
			e->onDeath(*this, id);
			e = get(id);
			if (e == nullptr)
				return;
		}
	}

	// The C works on a *local copy* of the flags (process.c:143), writing back
	// only after the hook runs -- how APPEARING survives a player ship's own
	// preprocess. Not cleared on the element itself until PostProcess (process.c:202).
	ElementFlags flags = e->flags;

	if (!any(flags & ElementFlags::Disappearing))
	{
		// What this element entered the frame as (the C's current.image), captured
		// before the hook -- the overlap-repair protocol reverts a turn made this
		// frame by putting these back (process.c:453-506).
		e->priorMask = e->mask;
		e->priorFacing = e->facing;

		if (any(flags & ElementFlags::Appearing))
		{
			// SetUpElement (process.c:117-126). BeamGeometry is exempt:
			// seeding next from current would collapse the beam to a point.
			if (!reg_.all_of<BeamGeometry>(id))
				e->next = e->current;
			if (reg_.all_of<PlayerShip>(id))
				flags &= ~ElementFlags::Appearing;  // the local, not the element
		}

		if (e->preProcess != nullptr && !any(flags & ElementFlags::Appearing))
		{
			e->preProcess(*this, id);
			e = get(id);
			if (e == nullptr)
				return;
			flags = e->flags;
		}

		// Motion gates on IGNORE_VELOCITY alone (process.c:163), so a spawned
		// element still moves its first frame. Integration ADDS to `next`
		// (process.c:172-173); the wrap happens at commit, not here -- design-notes D4.
		if (!reg_.all_of<IgnoreVelocity>(id))
		{
			const Vec2i delta = e->velocity.advance(1);
			e->next = Vec2i{e->next.x + delta.x, e->next.y + delta.y};
		}

		// Unconditional once FINITE_LIFE is set (process.c:180-181): it cannot
		// underflow, because reaching zero routes through the death branch
		// above and skips this whole block.
		if (any(flags & ElementFlags::FiniteLife))
			--e->lifeSpan;
	}

	e->flags = (flags
					   & ~(ElementFlags::PostProcessed | ElementFlags::Collided))
			| ElementFlags::PreProcessed;
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
Battle::resolveAgainst(EntityId elemId, EntityId testId, EntityId succ,
		TimeValue maxTime, ElementFlags processedMask)
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

		const bool eTurned = e->mask != e->priorMask;
		const bool tTurned = t->mask != t->priorMask;
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
			e->mask = e->priorMask;
			e->facing = e->priorFacing;
		}
		if (tTurned)
		{
			t->mask = t->priorMask;
			t->facing = t->priorFacing;
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
				&& processCollisions(elemId, succ, earlier, processedMask))
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
			if (processCollisions(testId, from, earlier, processedMask))
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
		processCollisions(elemId, front(), kMaxTimeValue,
				processedMask);
		processCollisions(testId, front(), kMaxTimeValue,
				processedMask);
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

// ProcessCollisions (process.c:361-627): walks candidates from `first`,
// preprocessing stragglers via processedMask (process_flags) -- see
// design-notes D1. Returns whether `elem` ended the walk stopped.
bool
Battle::processCollisions(EntityId elemId, EntityId first, TimeValue maxTime,
		ElementFlags processedMask)
{
	for (EntityId testId = first; testId != kNoEntity;)
	{
		{
			// The walk preprocesses each element before testing the pair
			// (process.c:371-373): otherwise `next` is still last frame's
			// position and the sweep hits a ghost.
			auto t = get(testId);
			if (t == nullptr)
				break;
			if (!any(t->flags & processedMask))
				preProcessOne(testId);
		}

		// Fetched after the preprocess, as the C fetches hSuccElement after
		// PreProcess (process.c:374), so a spawn made there is walked into.
		const EntityId succ = next(testId);

		if (!(testId == elemId)
				&& resolveAgainst(elemId, testId, succ, maxTime, processedMask))
			return true;

		testId = succ;
	}

	auto e = get(elemId);
	return e == nullptr || any(e->flags & ElementFlags::Collided);
}

void
Battle::preProcessPass()
{
	// A LIVE walk, not a snapshot (process.c:630-746) -- see design-notes D1.
	// Safe because nothing removes an element mid-frame: death only marks
	// Disappearing, and the reap happens in the post pass.
	for (EntityId id = front(); id != kNoEntity;)
	{
		auto e = get(id);
		if (e != nullptr && !any(e->flags & ElementFlags::PreProcessed))
			preProcessOne(id);

		e = get(id);
		if (e != nullptr && e->collidable()
				&& !any(e->flags & ElementFlags::Collided))
		{
			// Successors only, so each pair is visited once per frame -- the
			// C passes GetSuccElement for exactly this reason (process.c:667).
			(void)processCollisions(id, next(id), kMaxTimeValue,
					ElementFlags::PreProcessed);
		}

		// Fetched after the hooks so a tail insertion is walked into.
		id = next(id);
	}
}

void
Battle::catchUpFrom(EntityId first)
{
	// The mid-frame-spawn catch-up (process.c:843-862): integrates and tests
	// new elements against the WHOLE list, live like the outer walk -- see
	// design-notes D1. Gated on PreProcessed|PostProcessed (process.c:859).
	constexpr ElementFlags kDone =
			ElementFlags::PreProcessed | ElementFlags::PostProcessed;

	for (EntityId p = first; p != kNoEntity;)
	{
		auto pe = get(p);
		if (pe != nullptr && !any(pe->flags & kDone))
			preProcessOne(p);

		pe = get(p);
		if (pe != nullptr && pe->collidable()
				&& !any(pe->flags & ElementFlags::Collided))
			(void)processCollisions(p, front(), kMaxTimeValue, kDone);

		p = next(p);
	}
}

void
Battle::postProcessPass()
{
	// The C's PostProcessQueue (process.c:798-983), drawing removed, a LIVE
	// walk like PreProcessQueue -- see design-notes D1. A weapon fired by a
	// postprocess hook is appended, reached, and committed this same walk.
	for (EntityId id = front(); id != kNoEntity;)
	{
		auto e = get(id);
		EntityId nextId = kNoEntity;

		if (!any(e->flags & ElementFlags::PreProcessed))
		{
			catchUpFrom(id);
			e = get(id);
		}
		else if (!any(e->flags & ElementFlags::Collided))
		{
			// A frame without a collision ends DefyPhysics (process.c:824-827): it
			// has to expire, or the first stationary contact disables the collision
			// stagger for good, and later contacts fall into the stuck-pair branch.
			e->flags &= ~ElementFlags::DefyPhysics;
		}

		if (any(e->flags & ElementFlags::Disappearing))
		{
			// Removed with no postprocess and no commit (process.c:873-879).
			// The death hook already ran in the pre pass, while the element
			// could still be looked at. destroy() reaps every component with
			// the entity -- ShipState, guidance, the app's Visual.
			nextId = next(id);
			removeElement(id);
		}
		else
		{
			// PostProcess (process.c:188-204): the hook, then the commit.
			if (e->postProcess != nullptr
					&& !any(e->flags & ElementFlags::PostProcessed))
			{
				e->postProcess(*this, id);
				e = get(id);
			}

			if (e != nullptr)
			{
				// The wrap lives here, at the commit (process.c:899-916) -- see
				// design-notes D4. BeamGeometry is exempt: its two points are
				// the beam, not motion.
				if (!reg_.all_of<BeamGeometry>(id))
				{
					e->next = wrap(e->next);
					e->current = e->next;
				}

				// PostProcessed is POST_PROCESS: marks "had its frame" so whole-list
				// walks don't integrate it twice (design-notes D1). Ageing/death die
				// at frame start after life hits zero (process.c:133-141, 180-181).
				e->flags = (e->flags
								   & ~(ElementFlags::Appearing
										   | ElementFlags::PreProcessed))
						| ElementFlags::PostProcessed;
			}

			// Fetched after the hook, so a tail spawn is walked into.
			nextId = next(id);
		}

		id = nextId;
	}
}

void
Battle::step()
{
	collisions_.clear();
	spawns_.clear();
	preProcessPass();
	postProcessPass();
	++frame_;
}

}  // namespace uqm::sim
