// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Battle.hpp"

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

void
Battle::integrate(EntityId id) noexcept
{
	Element *e = elements_.get(id);
	if (e == nullptr)
		return;

	if (any(e->flags & ElementFlags::Appearing))
	{
		// Newly spawned: it has a position but no motion yet, so `next` is
		// seeded rather than advanced. process.c:379 does the same
		// (`EPtr->next = EPtr->current`).
		e->next = e->current;
		return;
	}

	if (any(e->flags & ElementFlags::DefyPhysics))
	{
		// Something else is placing it this frame -- a tractor beam, a
		// turret following its ship. The flag is cleared in the post pass.
		e->next = e->current;
		return;
	}

	const Vec2i delta = e->velocity.advance(1);
	e->next = wrap(Vec2i{e->current.x + delta.x, e->current.y + delta.y});
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
		{
			if (e->preProcess != nullptr)
				e->preProcess(*this, id);

			e = elements_.get(id);
			if (e == nullptr)
				continue;
			e->flags |= ElementFlags::PreProcessed;
			integrate(id);
		}

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

		if (e->preProcess != nullptr)
			e->preProcess(*this, id);
		e = elements_.get(id);
		if (e == nullptr)
			continue;
		e->flags |= ElementFlags::PreProcessed;
		integrate(id);

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

		e->current = e->next;

		if (any(e->flags & ElementFlags::FiniteLife)
				&& !any(e->flags & ElementFlags::Disappearing))
		{
			if (e->lifeSpan > 0)
				--e->lifeSpan;
			if (e->lifeSpan == 0)
				e->flags |= ElementFlags::Disappearing;
		}

		// Per-frame flags reset here, not at the top of the next step, so a
		// hook that inspects them after the fact sees this frame's values.
		e->flags &= ~(ElementFlags::Appearing | ElementFlags::Collided
				| ElementFlags::PreProcessed | ElementFlags::PostProcessed
				| ElementFlags::DefyPhysics);
	}

	scratch_.clear();
	for (EntityId id = elements_.front(); id.valid(); id = elements_.next(id))
	{
		const Element *e = elements_.get(id);
		if (e != nullptr && any(e->flags & ElementFlags::Disappearing))
			scratch_.push_back(id);
	}
	for (const EntityId id : scratch_)
	{
		Element *e = elements_.get(id);
		if (e != nullptr && e->onDeath != nullptr)
			e->onDeath(*this, id);
		elements_.remove(id);
	}
}

void
Battle::step()
{
	preProcessPass();
	postProcessPass();
	++frame_;
}

}  // namespace uqm::sim
