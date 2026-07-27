// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Battle.hpp"

#include "sim/Impulse.hpp"
#include "sim/World.hpp"

#include <utility>

namespace uqm::sim {

EntityId
Battle::spawnFront(Element e)
{
	e.flags |= ElementFlags::Appearing;
	return elements_.pushFront(std::move(e));
}

EntityId
Battle::spawnBack(Element e)
{
	e.flags |= ElementFlags::Appearing;
	return elements_.pushBack(std::move(e));
}

// PreProcess (process.c:128-186), which is more than "call the hook".
//
// The ordering here was got wrong once and the tests caught it, so it is
// worth spelling out. A newly spawned element is seeded with next = current
// (SetUpElement, process.c:117-126) and then **still has its velocity
// applied** -- process.c:163 gates motion on IGNORE_VELOCITY and nothing
// else. Appearing suppresses the preprocess *hook*, not the movement. Getting
// that wrong costs every projectile its first frame of flight, which at 24 Hz
// is a visible stutter at the muzzle.
//
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
	// FINITE_LIFE guard. That is what lets do_damage kill an asteroid by
	// assigning life_span = 0 (misc.c:210,221) even though an asteroid does
	// not age. It is also why a persistent element is born with NORMAL_LIFE
	// rather than 0: a zero would be read as "died last frame".
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

	// From here the C works on a *local copy* of the flags (process.c:143) and
	// writes it back at the end, re-reading from the element only after the
	// preprocess hook has run. That is not a detail: it is how APPEARING
	// survives a player ship's own preprocess. The flag is cleared in the
	// local at line 151 purely so the `!(state_flags & APPEARING)` test at 154
	// lets the hook run, then line 158 reads it straight back off the element.
	// APPEARING is not actually cleared until PostProcess (process.c:202).
	ElementFlags flags = e->flags;

	if (!any(flags & ElementFlags::Disappearing))
	{
		if (any(flags & ElementFlags::Appearing))
		{
			e->next = e->current;  // SetUpElement (process.c:117-126)
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

		// Motion is gated on IGNORE_VELOCITY alone (process.c:163). A newly
		// spawned element still moves on its first frame; APPEARING suppresses
		// the hook, not the movement.
		if (!any(flags & ElementFlags::IgnoreVelocity))
		{
			const Vec2i delta = e->velocity.advance(1);
			e->next = wrap(
					Vec2i{e->current.x + delta.x, e->current.y + delta.y});
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

bool
Battle::testPair(EntityId aId, EntityId bId)
{
	auto a = elements_.get(aId);
	auto b = elements_.get(bId);
	if (a == nullptr || b == nullptr)
		return false;
	if (!a->collidable() || !b->collidable())
		return false;

	// IGNORE_SIMILAR: same owner, same kind. A flame stream must not eat
	// itself, but a flame must still hit an asteroid.
	const bool similar = a->playerNr == b->playerNr && a->kind == b->kind;
	if (similar
			&& (any(a->flags & ElementFlags::IgnoreSimilar)
					|| any(b->flags & ElementFlags::IgnoreSimilar)))
		return false;

	// Masks are measured in display pixels and positions are in world units,
	// so the conversion is not optional -- feeding world coordinates to the
	// intersect test makes everything four times further apart than it is and
	// nothing ever touches. The C converts at exactly this boundary, in
	// InitIntersectStartPoint/EndPoint (collide.h:44-54).
	const Body ba{a->mask, worldToDisplay(a->current), worldToDisplay(a->next)};
	const Body bb{b->mask, worldToDisplay(b->current), worldToDisplay(b->next)};
	const Impact hit = sweptIntersect(ba, bb);
	if (!hit)
		return false;

	// Both stop where they met -- but placed in *world* units, by rewinding
	// their own motion to the impact time, not by converting the impact point
	// back from display space.
	//
	// Converting back looked equivalent and was not. displayToWorld multiplies
	// by four, so it snapped every collision to a four-unit grid and dragged
	// each body backwards by up to three world units. Two ships in contact
	// were pulled together again every frame by as much as the impulse pushed
	// them apart, so they stuck instead of separating -- and the minimum-nudge
	// in applyImpulse then fought the same battle from the other side.
	//
	// The sub-frame time is exact and already computed, so interpolating the
	// original world-space motion keeps full precision.
	const std::int32_t t = static_cast<std::int32_t>(hit.time) - 1;  // 0..256
	const auto rewind = [t](Vec2i from, Vec2i to) {
		return Vec2i{from.x
					+ static_cast<std::int32_t>(
							(std::int64_t{to.x - from.x} * t) >> kTimeShift),
				from.y
						+ static_cast<std::int32_t>(
								(std::int64_t{to.y - from.y} * t) >> kTimeShift)};
	};
	const Vec2i aNext = rewind(a->current, a->next);
	const Vec2i bNext = rewind(b->current, b->next);
	a->next = aNext;
	b->next = bNext;
	a->collidedWith = bId;
	b->collidedWith = aId;
	a->flags |= ElementFlags::Collided;
	b->flags |= ElementFlags::Collided;

	// Momentum exchange. Weapons do not bounce -- they are resolved by
	// whoever owns them, from the collidedWith they were just given.
	if (a->kind != ElementKind::Weapon && b->kind != ElementKind::Weapon)
		applyImpulse(*a, *b);

	// Then each side's collision hook, which is where damage happens. Both
	// run, and each reads its own `collidedWith` -- the C calls collision_func
	// on both elements too. Re-fetched around every call because a hook can
	// remove things, including itself.
	const ElementHook aHook = a->onCollision;
	const ElementHook bHook = b->onCollision;
	if (aHook != nullptr)
		aHook(*this, aId);
	if (bHook != nullptr && elements_.get(bId) != nullptr)
		bHook(*this, bId);
	return true;
}

void
Battle::collideAgainstSuccessors(EntityId id)
{
	// Only successors, so each pair is visited once per frame -- the C walks
	// from hNextElement for exactly this reason (process.c:667).
	for (EntityId other = elements_.next(id); other.valid();
			other = elements_.next(other))
	{
		if (testPair(id, other))
			return;  // already resolved; the C breaks out too
	}
}

void
Battle::collideAgainstAll(EntityId id)
{
	// The catch-up path for an element spawned mid-frame: everything already
	// walked past it, so it has to be tested against the whole list rather
	// than just what follows (process.c:858).
	for (EntityId other = elements_.front(); other.valid();
			other = elements_.next(other))
	{
		if (other == id)
			continue;
		if (testPair(id, other))
			return;
	}
}

void
Battle::preProcessPass()
{
	// Snapshot the ids first. Hooks spawn elements, and a spawn at the head
	// would otherwise be walked into in the same pass -- which is what the
	// post pass exists to handle deliberately rather than by accident.
	scratch_.clear();
	for (EntityId id = elements_.front(); id.valid(); id = elements_.next(id))
		scratch_.push_back(id);

	for (const EntityId id : scratch_)
	{
		auto e = elements_.get(id);
		if (e == nullptr)
			continue;

		if (!any(e->flags & ElementFlags::PreProcessed))
			preProcessOne(id);

		e = elements_.get(id);
		if (e != nullptr && e->collidable()
				&& !any(e->flags & ElementFlags::Collided))
			collideAgainstSuccessors(id);
	}
}

void
Battle::postProcessPass()
{
	// Anything spawned during the pre pass has no PreProcessed flag. Give it
	// the catch-up treatment before the reap, so a weapon created this frame
	// can still hit something this frame.
	scratch_.clear();
	for (EntityId id = elements_.front(); id.valid(); id = elements_.next(id))
		scratch_.push_back(id);

	for (const EntityId id : scratch_)
	{
		auto e = elements_.get(id);
		if (e == nullptr || any(e->flags & ElementFlags::PreProcessed))
			continue;

		preProcessOne(id);

		e = elements_.get(id);
		if (e != nullptr && e->collidable()
				&& !any(e->flags & ElementFlags::Collided))
			collideAgainstAll(id);
	}

	// Commit, age, and reap.
	scratch_.clear();
	for (EntityId id = elements_.front(); id.valid(); id = elements_.next(id))
		scratch_.push_back(id);

	for (const EntityId id : scratch_)
	{
		auto e = elements_.get(id);
		if (e == nullptr)
			continue;

		if (e->postProcess != nullptr && !any(e->flags & ElementFlags::PostProcessed))
		{
			e->postProcess(*this, id);
			e = elements_.get(id);
			if (e == nullptr)
				continue;
		}

		// PostProcess (process.c:189-193): the hook, then commit.
		e->current = e->next;

		// Ageing and the death decision both live in the pre pass, matching
		// process.c:133-141 and 180-181, so an element dies at the start of
		// the frame after its life reached zero -- not at the end of the one
		// that spent it.
		e->flags &= ~(ElementFlags::Appearing | ElementFlags::PreProcessed
				| ElementFlags::PostProcessed);
	}

	scratch_.clear();
	for (EntityId id = elements_.front(); id.valid(); id = elements_.next(id))
	{
		auto e = elements_.get(id);
		if (e != nullptr && any(e->flags & ElementFlags::Disappearing))
			scratch_.push_back(id);
	}
	// The death hook already ran in the pre pass, while the element was still
	// in the list and could still be looked at.
	for (const EntityId id : scratch_)
		elements_.remove(id);
}

void
Battle::step()
{
	preProcessPass();
	postProcessPass();
	++frame_;
}

}  // namespace uqm::sim
