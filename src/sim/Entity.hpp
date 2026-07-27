// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_ENTITY_HPP
#define UQM2_SIM_ENTITY_HPP

#include <entt/entity/entity.hpp>

namespace uqm::sim {

// The entity handle: entt's versioned integer id -- the old
// EntityId{index, generation} packed into one word, with the same promise:
// a stale handle compares unequal and reads as dead, never as the next
// tenant (review-004 §2).
using EntityId = entt::entity;

// The no-entity value. entt::null never matches a live entity, like the
// old generation-0 handle.
inline constexpr EntityId kNoEntity = entt::null;

// The C's disp_q, as data: the registry stores, this orders. Traversal
// order is gameplay (sim-architecture.md) -- head-inserts preprocess
// before their inserter's successors, tail spawns are walked into the same
// frame -- so the order is an explicitly-kept doubly-linked spine, not a
// property of any pool.
//
// in_place_delete: links are read during the reap walk; a swap-and-pop
// move of someone else's links mid-walk would tear the chain.
struct OrderLink
{
	static constexpr auto in_place_delete = true;

	EntityId prev = kNoEntity;
	EntityId next = kNoEntity;
};

}  // namespace uqm::sim

#endif  // UQM2_SIM_ENTITY_HPP
