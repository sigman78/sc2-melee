// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Battle.hpp"

#include "sim/Damage.hpp"
#include "sim/Impulse.hpp"
#include "sim/World.hpp"

#include <utility>

namespace uqm::sim {

namespace {

// CollisionPossible (collide.h:34-39), which is stricter than it looks.
//
// The TEST element must be collidable -- the scanner's own collidability is
// the walk's business, checked before and after each resolution. The pair is
// skipped when BOTH are already stopped this frame, when both carry
// IGNORE_SIMILAR and share an owner (both halves matter: `both` not `either`,
// and `owner` not `player` or `kind` -- testing kind is what once let an
// Ilwrath flame burn the Avenger that breathed it), and when neither side has
// mass: a weapon's mass is its damage, and two massless things have nothing
// to resolve.
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

// Masks are measured in display pixels and positions are in world units, so
// the conversion is not optional -- feeding world coordinates to the
// intersect test makes everything four times further apart than it is and
// nothing ever touches. The C converts at exactly this boundary, in
// InitIntersectStartPoint/EndPoint (collide.h:44-54).
[[nodiscard]] Body
bodyOf(const Element &e) noexcept
{
	return Body{e.mask, worldToDisplay(e.current), worldToDisplay(e.next)};
}

// Where a body stops if its hook says so -- placed in *world* units, by
// rewinding its own motion to the impact time, not by converting the impact
// point back from display space.
//
// Converting back looked equivalent and was not. displayToWorld multiplies by
// four, so it snapped every collision to a four-unit grid and dragged each
// body backwards by up to three world units. Two ships in contact were pulled
// together again every frame by as much as the impulse pushed them apart, so
// they stuck instead of separating -- and the minimum-nudge in applyImpulse
// then fought the same battle from the other side. This is a deliberate
// precision divergence from the C's DISPLAY_TO_WORLD(SavePt)
// (process.c:578-595).
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
		// What this element entered the frame as -- the C's current.image.
		// The overlap-repair protocol reverts a turn made this frame by
		// putting these back (process.c:453-506); captured before the hook,
		// which is where turning happens.
		e->priorMask = e->mask;
		e->priorFacing = e->facing;

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

// The other half of "BAD NEWS": an APPEARING element wedged inside something
// on its spawn frame dies on the spot (process.c:427-449) -- full damage,
// then straight to DISAPPEARING with its death hook run now, so an asteroid
// respawned into the planet becomes rubble immediately instead of drifting
// through it. hit_points is crew for a ship -- the union (element.h:126-133).
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

	// A transient element does not collide on the frame it spawns
	// (process.c:389-394). A missile is born at its ship's muzzle; test it
	// there and it detonates on its own launcher. The exemption is the C's
	// shape exactly: FINITE_LIFE on EITHER side, and APPEARING-with-life-left
	// on EITHER side -- not necessarily the same element, which is what
	// exempts the planet (APPEARING, non-finite, life 2) against a weapon on
	// its spawn frame. The lifeSpan > 1 exception keeps point-defence fire
	// working: born with one frame to live, the spawn frame is the only one
	// it has, and preprocess has already decremented, same as in the C.
	if (any((e->flags | t->flags) & ElementFlags::FiniteLife)
			&& ((any(e->flags & ElementFlags::Appearing) && e->lifeSpan > 1)
					|| (any(t->flags & ElementFlags::Appearing)
							&& t->lifeSpan > 1)))
		return false;

	const bool bothSolid =
			!any((e->flags | t->flags) & ElementFlags::FiniteLife);
	Impact hit = sweptIntersect(bodyOf(*e), bodyOf(*t), maxTime);

	// "BAD NEWS" (process.c:397-516). Impact at time 1 between two solids is
	// a standing overlap, not a new collision, and gets a repair protocol
	// rather than a response -- responding again is how ships weld together.
	// Weapons are exempt from all of it: a shot that starts inside its target
	// has simply hit it.
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
			// Neither silhouette changed this frame: either a spawn wedged
			// inside something, which dies on the spot, or the tail of an
			// already-resolved contact, which is skipped so the impulse from
			// the original impact can carry the pair apart
			// (process.c:427-451, 509-515).
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

		// A silhouette changed into the overlap -- something rotated into a
		// wall. Undo the turn (process.c:453-506 reverts next.image to
		// current.image and re-reads ShipFacing from it) and ask again: with
		// the old silhouette back, the sweep may find no contact at all, a
		// genuine mid-frame impact, or a standing overlap that the branch
		// above then settles.
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

	// Earliest-collision-wins (process.c:531-540): before resolving this pair
	// at `hit.time`, ask whether either party hits something else EARLIER.
	// The recursion does not merely ask -- it resolves what it finds -- and a
	// yes on either side abandons this pair for the frame. Short-circuit
	// order is the C's: the scanner's side first.
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

	// Keep scanning unless the scanner is now out of the game for the frame
	// (process.c:609-618): stopped, or no longer collidable -- a ship merely
	// hit by a missile is neither, and can still bounce off another ship
	// this same frame.
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

// ProcessCollisions (process.c:361-627): walk the candidates from `first`,
// preprocessing anything the frame has not touched yet, and resolve what
// `elem` hits. `processedMask` is the C's process_flags -- PreProcessed in
// the pre pass, PreProcessed|PostProcessed in the post pass, where the
// second flag is what stops a committed element being integrated twice by a
// whole-list walk. Returns whether `elem` ended the walk stopped.
bool
Battle::processCollisions(EntityId elemId, EntityId first, TimeValue maxTime,
		ElementFlags processedMask)
{
	for (EntityId testId = first; testId.valid();)
	{
		{
			// The walk preprocesses each element it passes, before the pair
			// is tested (process.c:371-373). This is what makes the test see
			// the other side's motion for *this* frame: its `next` is
			// otherwise still last frame's position and the sweep hits a
			// ghost.
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
	// The mid-frame-spawn catch-up (process.c:843-862): from the first
	// element the post walk found un-preprocessed, run to the tail,
	// integrating and ageing everything new and collision-testing against
	// the WHOLE list -- everything ahead of it already moved, so successors
	// alone would miss most partners. This is what lets a one-frame weapon
	// -- point-defence fire -- hit something on the only frame it has, and
	// what gives a missile its first frame of flight on the frame the
	// trigger was pulled. Live, like the outer walk: a weapon spawned by an
	// element this loop preprocesses is reached by this same loop.
	//
	// The gate is PreProcessed OR PostProcessed, the C's PRE_PROCESS |
	// POST_PROCESS (process.c:859): a committed element has already had its
	// frame, and integrating it again from a whole-list walk would move and
	// age it twice.
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

				// PostProcessed is the C's POST_PROCESS flag: it marks "had
				// its frame", so the whole-list walks above do not integrate
				// a committed element a second time. The next preprocess
				// clears it. Ageing and the death decision both live in the
				// pre pass, matching process.c:133-141 and 180-181, so an
				// element dies at the start of the frame after its life
				// reached zero -- not at the end of the one that spent it.
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
