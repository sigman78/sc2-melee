// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Battle.hpp"

#include "sim/Impulse.hpp"
#include "sim/World.hpp"

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
	Element *e = elements_.get(id);
	if (e == nullptr)
		return;

	// Death is decided at the *start* of the frame after life reached zero,
	// and the hook runs while the element is still in the list
	// (process.c:133-141).
	if (any(e->flags & ElementFlags::FiniteLife) && e->lifeSpan == 0)
	{
		e->flags |= ElementFlags::Disappearing;
		if (e->onDeath != nullptr)
			e->onDeath(*this, id);
		e = elements_.get(id);
		if (e == nullptr)
			return;
	}

	if (!any(e->flags & ElementFlags::Disappearing))
	{
		const bool appearing = any(e->flags & ElementFlags::Appearing);
		if (appearing)
		{
			e->next = e->current;  // SetUpElement
			if (any(e->flags & ElementFlags::PlayerShip))
				e->flags &= ~ElementFlags::Appearing;
		}

		if (e->preProcess != nullptr && !appearing)
		{
			e->preProcess(*this, id);
			e = elements_.get(id);
			if (e == nullptr)
				return;
		}
		else if (appearing && any(e->flags & ElementFlags::PlayerShip)
				&& e->preProcess != nullptr)
		{
			// A ship's hook *does* run on its appearing frame -- the C clears
			// the flag in a local before calling, so ship_preprocess still
			// sees APPEARING on the element and takes its init branch.
			e->flags |= ElementFlags::Appearing;
			e->preProcess(*this, id);
			e = elements_.get(id);
			if (e == nullptr)
				return;
			e->flags &= ~ElementFlags::Appearing;
		}

		if (!any(e->flags & ElementFlags::IgnoreVelocity))
		{
			const Vec2i delta = e->velocity.advance(1);
			e->next = wrap(
					Vec2i{e->current.x + delta.x, e->current.y + delta.y});
		}

		if (any(e->flags & ElementFlags::FiniteLife) && e->lifeSpan > 0)
			--e->lifeSpan;
	}

	e->flags &= ~ElementFlags::Collided;
	e->flags |= ElementFlags::PreProcessed;
}

bool
Battle::testPair(EntityId aId, EntityId bId)
{
	Element *a = elements_.get(aId);
	Element *b = elements_.get(bId);
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

	const Body ba{a->mask, a->current, a->next};
	const Body bb{b->mask, b->current, b->next};
	const Impact hit = sweptIntersect(ba, bb);
	if (!hit)
		return false;

	// Both stop where they met, which is what the C writes back through
	// EndPoint, and both are marked so neither is tested again this frame.
	a->next = hit.at0;
	b->next = hit.at1;
	a->collidedWith = bId;
	b->collidedWith = aId;
	a->flags |= ElementFlags::Collided;
	b->flags |= ElementFlags::Collided;

	// Momentum exchange. Weapons do not bounce -- they are resolved by
	// whoever owns them, from the collidedWith they were just given.
	if (a->kind != ElementKind::Weapon && b->kind != ElementKind::Weapon)
		applyImpulse(*a, *b);
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
		Element *e = elements_.get(id);
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
		Element *e = elements_.get(id);
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
		Element *e = elements_.get(id);
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
		const Element *e = elements_.get(id);
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
