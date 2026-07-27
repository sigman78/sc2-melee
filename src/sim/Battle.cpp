// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Battle.hpp"

#include "sim/Damage.hpp"
#include "sim/Impulse.hpp"
#include "sim/World.hpp"

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
			&& test.owner.valid() && test.owner == elem.owner)
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

void
Battle::recordSpawn(EntityId id, const Element &e)
{
	spawns_.push_back(SpawnEvent{id, e.kind, e.playerNr});
}

EntityId
Battle::spawnFront(Element e)
{
	e.flags |= ElementFlags::Appearing;
	const EntityId id = elements_.pushFront(std::move(e));
	if (auto p = elements_.get(id); p != nullptr)
		recordSpawn(id, *p);
	return id;
}

EntityId
Battle::spawnBack(Element e)
{
	e.flags |= ElementFlags::Appearing;
	const EntityId id = elements_.pushBack(std::move(e));
	if (auto p = elements_.get(id); p != nullptr)
		recordSpawn(id, *p);
	return id;
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
	auto e = elements_.get(id);
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
			e = elements_.get(id);
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
			if (!any(flags & ElementFlags::BeamGeometry))
				e->next = e->current;
			if (any(flags & ElementFlags::PlayerShip))
				flags &= ~ElementFlags::Appearing;  // the local, not the element
		}

		if (e->preProcess != nullptr && !any(flags & ElementFlags::Appearing))
		{
			e->preProcess(*this, id);
			e = elements_.get(id);
			if (e == nullptr)
				return;
			flags = e->flags;
		}

		// Motion gates on IGNORE_VELOCITY alone (process.c:163), so a spawned
		// element still moves its first frame. Integration ADDS to `next`
		// (process.c:172-173); the wrap happens at commit, not here -- design-notes D4.
		if (!any(flags & ElementFlags::IgnoreVelocity))
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
	auto e = elements_.get(id);
	if (e == nullptr)
		return;

	doDamage(*e, any(e->flags & ElementFlags::PlayerShip) ? e->ship.crew
														  : e->hitPoints);
	e = elements_.get(id);
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
	auto e = elements_.get(elemId);
	if (e == nullptr)
		return true;
	auto t = elements_.get(testId);
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
		e = elements_.get(elemId);
		t = elements_.get(testId);
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
					? elements_.front()
					: elements_.next(elemId);
			if (processCollisions(testId, from, earlier, processedMask))
				return false;
		}
		e = elements_.get(elemId);
		t = elements_.get(testId);
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
	if (any(t->flags & ElementFlags::PlayerShip))
	{
		if (tHook != nullptr)
			tHook(*this, testId);
		if (eHook != nullptr && elements_.get(elemId) != nullptr)
			eHook(*this, elemId);
	}
	else
	{
		if (eHook != nullptr)
			eHook(*this, elemId);
		if (tHook != nullptr && elements_.get(testId) != nullptr)
			tHook(*this, testId);
	}

	e = elements_.get(elemId);
	t = elements_.get(testId);

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
			applyImpulse(*e, *t);
			impulsed = true;
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
		processCollisions(elemId, elements_.front(), kMaxTimeValue,
				processedMask);
		processCollisions(testId, elements_.front(), kMaxTimeValue,
				processedMask);
	}

	// Keeps scanning unless out of the game for the frame (process.c:609-618):
	// stopped, or no longer collidable -- a ship merely hit by a missile is
	// neither, and can still bounce off another ship this frame.
	e = elements_.get(elemId);
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
	for (EntityId testId = first; testId.valid();)
	{
		{
			// The walk preprocesses each element before testing the pair
			// (process.c:371-373): otherwise `next` is still last frame's
			// position and the sweep hits a ghost.
			auto t = elements_.get(testId);
			if (t == nullptr)
				break;
			if (!any(t->flags & processedMask))
				preProcessOne(testId);
		}

		// Fetched after the preprocess, as the C fetches hSuccElement after
		// PreProcess (process.c:374), so a spawn made there is walked into.
		const EntityId succ = elements_.next(testId);

		if (!(testId == elemId)
				&& resolveAgainst(elemId, testId, succ, maxTime, processedMask))
			return true;

		testId = succ;
	}

	auto e = elements_.get(elemId);
	return e == nullptr || any(e->flags & ElementFlags::Collided);
}

void
Battle::preProcessPass()
{
	// A LIVE walk, not a snapshot (process.c:630-746) -- see design-notes D1.
	// Safe because nothing removes an element mid-frame: death only marks
	// Disappearing, and the reap happens in the post pass.
	for (EntityId id = elements_.front(); id.valid();)
	{
		auto e = elements_.get(id);
		if (e != nullptr && !any(e->flags & ElementFlags::PreProcessed))
			preProcessOne(id);

		e = elements_.get(id);
		if (e != nullptr && e->collidable()
				&& !any(e->flags & ElementFlags::Collided))
		{
			// Successors only, so each pair is visited once per frame -- the
			// C passes GetSuccElement for exactly this reason (process.c:667).
			(void)processCollisions(id, elements_.next(id), kMaxTimeValue,
					ElementFlags::PreProcessed);
		}

		// Fetched after the hooks so a tail insertion is walked into.
		id = elements_.next(id);
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

	for (EntityId p = first; p.valid();)
	{
		auto pe = elements_.get(p);
		if (pe != nullptr && !any(pe->flags & kDone))
			preProcessOne(p);

		pe = elements_.get(p);
		if (pe != nullptr && pe->collidable()
				&& !any(pe->flags & ElementFlags::Collided))
			(void)processCollisions(p, elements_.front(), kMaxTimeValue, kDone);

		p = elements_.next(p);
	}
}

void
Battle::postProcessPass()
{
	// The C's PostProcessQueue (process.c:798-983), drawing removed, a LIVE
	// walk like PreProcessQueue -- see design-notes D1. A weapon fired by a
	// postprocess hook is appended, reached, and committed this same walk.
	for (EntityId id = elements_.front(); id.valid();)
	{
		auto e = elements_.get(id);
		EntityId next;

		if (!any(e->flags & ElementFlags::PreProcessed))
		{
			catchUpFrom(id);
			e = elements_.get(id);
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
			// could still be looked at.
			next = elements_.next(id);
			elements_.remove(id);
		}
		else
		{
			// PostProcess (process.c:188-204): the hook, then the commit.
			if (e->postProcess != nullptr
					&& !any(e->flags & ElementFlags::PostProcessed))
			{
				e->postProcess(*this, id);
				e = elements_.get(id);
			}

			if (e != nullptr)
			{
				// The wrap lives here, at the commit (process.c:899-916) -- see
				// design-notes D4. BeamGeometry is exempt: its two points are
				// the beam, not motion.
				if (!any(e->flags & ElementFlags::BeamGeometry))
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
			next = elements_.next(id);
		}

		id = next;
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
