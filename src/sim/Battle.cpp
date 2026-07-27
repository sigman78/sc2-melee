// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Battle.hpp"

#include "sim/Impulse.hpp"
#include "sim/World.hpp"

#include <utility>

namespace uqm::sim {

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
			// SetUpElement (process.c:117-126). A laser is exempt: its
			// `current` and `next` are the two ENDS of the beam rather than a
			// position and a destination, and seeding next from current would
			// collapse it to a point before its one frame on screen.
			if (e->kind != ElementKind::Laser)
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

		// Motion is gated on IGNORE_VELOCITY alone (process.c:163). A newly
		// spawned element still moves on its first frame; APPEARING suppresses
		// the hook, not the movement.
		//
		// Two details are the C's and both were once "tidied" away:
		//
		//   - Integration ADDS to `next` (process.c:172-173) rather than
		//     rebuilding it from `current`, so a hook that nudged `next`
		//     keeps its nudge (crew_preprocess positions drifting crew that
		//     way).
		//   - The result is NOT wrapped. The C wraps at the commit
		//     (process.c:899-916), so a seam-crossing element is collision-
		//     tested with raw coordinates: the sweep sees a short hop off the
		//     arena's edge. Wrapping here handed the sweep a full-arena
		//     traversal instead, which could manufacture phantom hits against
		//     anything near that path. The cost, also the C's: a genuine
		//     seam collision resolves one frame late.
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

bool
Battle::testPair(EntityId aId, EntityId bId)
{
	auto a = elements_.get(aId);
	auto b = elements_.get(bId);
	if (a == nullptr || b == nullptr)
		return false;
	if (!a->collidable() || !b->collidable())
		return false;

	// CollisionPossible (collide.h:34-39), which is stricter than it looks.
	//
	// The pair is skipped when *both* carry IGNORE_SIMILAR and they share an
	// owner. Both halves matter: `both` and not `either`, and `owner` and not
	// `player` or `kind`. Testing kind instead is what let an Ilwrath flame
	// burn the Avenger that breathed it -- a ship and a weapon are different
	// kinds, so the pair never matched and the hull took its own fire.
	const bool bothIgnore = any(a->flags & ElementFlags::IgnoreSimilar)
			&& any(b->flags & ElementFlags::IgnoreSimilar);
	if (bothIgnore && a->owner.valid() && a->owner == b->owner)
		return false;

	// And at least one side must have mass. Two massless things pass through
	// each other -- there is no momentum to exchange and nothing to resolve.
	if (a->mass == 0 && b->mass == 0)
		return false;

	// A transient element does not collide on the frame it spawns
	// (process.c:389-394). A missile is born at its ship's muzzle; test it
	// there and it detonates on its own launcher. The lifeSpan > 1 exception
	// keeps point-defence fire working: born with one frame to live, the
	// spawn frame is the only one it has. lifeSpan has already been
	// decremented by preprocess, same as in the C, so the comparison carries
	// over unchanged.
	const auto bornThisFrame = [](const Element &e) {
		return any(e.flags & ElementFlags::Appearing)
				&& any(e.flags & ElementFlags::FiniteLife) && e.lifeSpan > 1;
	};
	if (bornThisFrame(*a) || bornThisFrame(*b))
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

	// Solid means "on the field until something kills it" -- a ship, a rock,
	// the planet. FINITE_LIFE is where the C draws the same line, and the
	// whole response protocol below hangs off it.
	const bool aSolid = !any(a->flags & ElementFlags::FiniteLife);
	const bool bSolid = !any(b->flags & ElementFlags::FiniteLife);

	// Impact time 1 is "already overlapping before either moved". For two
	// solid bodies that is not a new collision, it is the tail of the previous
	// one, and responding again is how ships weld together: both would be
	// rewound to where they already stand, and the scrape-promotion in
	// applyImpulse would reflect their separating velocities back inward --
	// every frame, forever, since frozen ships can never stop overlapping.
	// The C detects exactly this ("BAD NEWS", process.c:397-416, 509-515) and
	// skips the pair, letting the impulse from the original impact carry them
	// apart. Weapons are exempt: a shot that starts inside its target has
	// simply hit it.
	if (hit.time == 1 && aSolid && bSolid)
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

	// Who actually stops where they met. In the C an element is moved to the
	// impact point only if its own collision_func raised COLLISION
	// (process.c:586-596), and a ship's does so only when the other party is
	// solid (ship.c:356-358). So solid-on-solid stops both and exchanges
	// momentum, while a ship hit by a missile keeps its full motion -- only
	// the missile stops, dies, and leaves its blast at the impact point.
	// Truncating the ship too reads on screen as the ship hanging in the
	// stream of fire, halted again by every shot that lands.
	const bool exchange = aSolid && bSolid;
	if (exchange || !aSolid)
	{
		a->next = aNext;
		a->flags |= ElementFlags::Collided;
	}
	if (exchange || !bSolid)
	{
		b->next = bNext;
		b->flags |= ElementFlags::Collided;
	}
	a->collidedWith = bId;
	b->collidedWith = aId;

	// In world units per frame, so consumers never see the packed fixed point.
	const auto worldVelocity = [](const Element &e) {
		const Vec2i v = e.velocity.current();
		return Vec2i{velocityToWorld(v.x), velocityToWorld(v.y)};
	};
	CollisionEvent event;
	event.a = aId;
	event.b = bId;
	event.at = aNext;
	event.beforeA = worldVelocity(*a);
	event.beforeB = worldVelocity(*b);

	// Momentum exchange is solid-on-solid only (process.c:598-601). A weapon
	// hit is resolved by damage, from the collidedWith it was just given.
	if (exchange)
		applyImpulse(*a, *b);

	event.afterA = worldVelocity(*a);
	event.afterB = worldVelocity(*b);
	collisions_.push_back(event);

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

	// Keep scanning unless this element is now out of the game for the frame.
	// The C stops the walk when the scanning element gains COLLISION or stops
	// being collidable (process.c:610-618); a ship merely hit by a missile is
	// neither, and can still bounce off another ship this same frame.
	a = elements_.get(aId);
	return a == nullptr || !a->collidable()
			|| any(a->flags & ElementFlags::Collided);
}

void
Battle::collideAgainstSuccessors(EntityId id)
{
	// Only successors, so each pair is visited once per frame -- the C walks
	// from hNextElement for exactly this reason (process.c:667).
	for (EntityId other = elements_.next(id); other.valid();
			other = elements_.next(other))
	{
		// The walk preprocesses each element it passes, before the pair is
		// tested (process.c:371-373). This is what makes the test see the
		// other side's motion for *this* frame: its `next` is otherwise still
		// last frame's position and the sweep hits a ghost. It is also what
		// makes a collision's effects on the other element stick -- a
		// preprocess run after the collision would re-integrate `next`,
		// wiping the truncation and the Collided flag along with it.
		auto o = elements_.get(other);
		if (o != nullptr && !any(o->flags & ElementFlags::PreProcessed))
			preProcessOne(other);

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

		// Same preprocess-before-test as the successor walk; an element
		// spawned even later than this one still needs its motion integrated
		// before the pair means anything.
		auto o = elements_.get(other);
		if (o != nullptr && !any(o->flags & ElementFlags::PreProcessed))
			preProcessOne(other);

		if (testPair(id, other))
			return;
	}
}

void
Battle::preProcessPass()
{
	// A LIVE walk, not a snapshot -- the C's PreProcessQueue follows the real
	// list (process.c:630-746), and the difference is observable: an element
	// a hook appends at the tail (rubble from a death, sparks from a burning
	// hull) is walked into by this same pass, preprocessed, and collision-
	// tested against its successors. An element head-inserted during the walk
	// lands behind the cursor and waits for the post pass's catch-up instead,
	// exactly as in the C. Snapshotting the ids up front deferred every
	// tail spawn too, which is not the C's shape.
	//
	// Safe to walk live because nothing removes an element mid-frame: death
	// only marks Disappearing, and the reap happens in the post pass.
	for (EntityId id = elements_.front(); id.valid();)
	{
		auto e = elements_.get(id);
		if (e != nullptr && !any(e->flags & ElementFlags::PreProcessed))
			preProcessOne(id);

		e = elements_.get(id);
		if (e != nullptr && e->collidable()
				&& !any(e->flags & ElementFlags::Collided))
			collideAgainstSuccessors(id);

		// Fetched after the hooks so a tail insertion is walked into.
		id = elements_.next(id);
	}
}

void
Battle::catchUpFrom(EntityId first)
{
	// The mid-frame-spawn catch-up (process.c:843-862): from the first
	// element the post walk found un-preprocessed, run to the tail,
	// integrating and ageing everything new and collision-testing against
	// the WHOLE list -- everything ahead of it already moved, so successors
	// alone would miss most partners. This is what lets a one-frame weapon
	// -- point-defence fire -- hit something on the only frame it has, and
	// what gives a missile its first frame of flight on the frame the
	// trigger was pulled. Live, like the outer walk: a weapon spawned by an
	// element this loop preprocesses is reached by this same loop.
	for (EntityId p = first; p.valid();)
	{
		auto pe = elements_.get(p);
		if (pe != nullptr && !any(pe->flags & ElementFlags::PreProcessed))
			preProcessOne(p);

		pe = elements_.get(p);
		if (pe != nullptr && pe->collidable()
				&& !any(pe->flags & ElementFlags::Collided))
			collideAgainstAll(p);

		p = elements_.next(p);
	}
}

void
Battle::postProcessPass()
{
	// The C's PostProcessQueue (process.c:798-983), with the drawing taken
	// out, and like it a LIVE walk. The load-bearing consequence: a weapon
	// fired by a ship's postprocess hook is appended at the tail, reached by
	// this same walk, caught up -- so it moves on the frame it was fired --
	// and committed, which is where its Appearing clears. Snapshotting here
	// cost every projectile its first frame of flight and left Appearing set
	// a frame late, which delayed its first possible hit by another frame.
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
			// A frame without a collision ends DefyPhysics
			// (process.c:824-827). It has to expire, or the first stationary
			// contact disables the collision stagger for good and later,
			// unrelated contacts fall into the zero-velocity branch meant
			// for pairs that are actually stuck.
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
				// The wrap lives here, at the commit, matching the C
				// (process.c:899-916) -- see the note in preProcessOne. A
				// laser is exempt from the commit entirely: its two points
				// are the beam, not motion.
				if (e->kind != ElementKind::Laser)
				{
					e->next = wrap(e->next);
					e->current = e->next;
				}

				// Ageing and the death decision both live in the pre pass,
				// matching process.c:133-141 and 180-181, so an element dies
				// at the start of the frame after its life reached zero --
				// not at the end of the one that spent it.
				e->flags &= ~(ElementFlags::Appearing
						| ElementFlags::PreProcessed
						| ElementFlags::PostProcessed);
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
