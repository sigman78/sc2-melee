// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_ENTITYLIST_HPP
#define UQM2_SIM_ENTITYLIST_HPP

#include <cassert>
#include <cstdint>
#include <vector>

namespace uqm::sim {

// A stable handle. The index addresses a slot; the generation makes a stale
// handle detectable rather than a silent read of whoever moved in.
struct EntityId
{
	std::uint32_t index = 0;
	std::uint32_t generation = 0;

	friend constexpr bool operator==(const EntityId &, const EntityId &) = default;

	[[nodiscard]] constexpr bool valid() const noexcept
	{
		return generation != 0;
	}
};

inline constexpr EntityId kNoEntity{};

// An ordered list of entities over a slot arena.
//
// Two structures rather than one, because the game needs both and they do not
// agree. The arena gives stable ids across insertion and removal; the
// intrusive order gives the traversal sequence, and **the order is
// observable behaviour, not a detail**:
//
//   - `disp_q` is spliced into the middle at 20 InsertElement sites, about 13
//     of them at the head;
//   - pkunk.c:498-512 head-inserts the phoenix specifically so it preprocesses
//     *before* the dead Pkunk's death_func runs, which is what starts the
//     reincarnation;
//   - the AI's target selection depends on traversal order.
//
// So an arena alone will not do: slot order is arbitrary the moment the free
// list reuses anything, and it would silently reorder the phoenix. Hence
// pushFront/insertAfter, and iteration that follows the links rather than the
// slots.
template <class T>
class EntityList
{
public:
	[[nodiscard]] std::size_t size() const noexcept { return count_; }
	[[nodiscard]] bool empty() const noexcept { return count_ == 0; }

	[[nodiscard]] EntityId front() const noexcept { return head_; }
	[[nodiscard]] EntityId back() const noexcept { return tail_; }

	[[nodiscard]] T *get(EntityId id) noexcept
	{
		return alive(id) ? &slots_[id.index].value : nullptr;
	}
	[[nodiscard]] const T *get(EntityId id) const noexcept
	{
		return alive(id) ? &slots_[id.index].value : nullptr;
	}

	[[nodiscard]] EntityId next(EntityId id) const noexcept
	{
		assert(alive(id) && "next() of a dead entity");
		return slots_[id.index].next;
	}
	[[nodiscard]] EntityId prev(EntityId id) const noexcept
	{
		assert(alive(id) && "prev() of a dead entity");
		return slots_[id.index].prev;
	}

	EntityId
	pushFront(T value)
	{
		const EntityId id = allocate(std::move(value));
		linkAfter(kNoEntity, id);
		return id;
	}

	EntityId
	pushBack(T value)
	{
		const EntityId id = allocate(std::move(value));
		linkAfter(tail_, id);
		return id;
	}

	// `after` may be kNoEntity, which means the head -- the same thing
	// InsertElement(h, GetHeadElement()) does.
	EntityId
	insertAfter(EntityId after, T value)
	{
		assert((!after.valid() || alive(after)) && "insert after a dead entity");
		const EntityId id = allocate(std::move(value));
		linkAfter(after, id);
		return id;
	}

	void
	remove(EntityId id)
	{
		if (!alive(id))
			return;

		Slot &slot = slots_[id.index];
		if (slot.prev.valid())
			slots_[slot.prev.index].next = slot.next;
		else
			head_ = slot.next;

		if (slot.next.valid())
			slots_[slot.next.index].prev = slot.prev;
		else
			tail_ = slot.prev;

		slot.value = T{};
		slot.prev = kNoEntity;
		slot.next = kNoEntity;
		// Bumping the generation is what turns a surviving handle into a
		// detectable mistake instead of a read of the next tenant.
		++slot.generation;
		slot.live = false;
		free_.push_back(id.index);
		--count_;
	}

	void
	clear() noexcept
	{
		for (std::uint32_t i = 0; i < slots_.size(); ++i)
		{
			if (slots_[i].live)
			{
				slots_[i].value = T{};
				slots_[i].live = false;
				++slots_[i].generation;
				free_.push_back(i);
			}
		}
		head_ = kNoEntity;
		tail_ = kNoEntity;
		count_ = 0;
	}

	// Reserving up front is the point of the arena: a battle's entity count
	// is bounded, so steady-state stepping should allocate nothing.
	void
	reserve(std::size_t n)
	{
		slots_.reserve(n);
		free_.reserve(n);
	}

	[[nodiscard]] bool
	alive(EntityId id) const noexcept
	{
		return id.valid() && id.index < slots_.size()
				&& slots_[id.index].live
				&& slots_[id.index].generation == id.generation;
	}

private:
	struct Slot
	{
		T value{};
		EntityId prev = kNoEntity;
		EntityId next = kNoEntity;
		// Starts at 1 so a default-constructed EntityId (generation 0) never
		// matches a live slot.
		std::uint32_t generation = 1;
		bool live = false;
	};

	EntityId
	allocate(T value)
	{
		std::uint32_t index;
		if (!free_.empty())
		{
			index = free_.back();
			free_.pop_back();
		}
		else
		{
			index = static_cast<std::uint32_t>(slots_.size());
			slots_.emplace_back();
		}

		Slot &slot = slots_[index];
		slot.value = std::move(value);
		slot.live = true;
		++count_;
		return EntityId{index, slot.generation};
	}

	void
	linkAfter(EntityId after, EntityId id)
	{
		Slot &slot = slots_[id.index];
		if (!after.valid())
		{
			slot.prev = kNoEntity;
			slot.next = head_;
			if (head_.valid())
				slots_[head_.index].prev = id;
			head_ = id;
			if (!tail_.valid())
				tail_ = id;
			return;
		}

		Slot &prevSlot = slots_[after.index];
		slot.prev = after;
		slot.next = prevSlot.next;
		if (prevSlot.next.valid())
			slots_[prevSlot.next.index].prev = id;
		else
			tail_ = id;
		prevSlot.next = id;
	}

	std::vector<Slot> slots_;
	std::vector<std::uint32_t> free_;
	EntityId head_ = kNoEntity;
	EntityId tail_ = kNoEntity;
	std::size_t count_ = 0;
};

}  // namespace uqm::sim

#endif  // UQM2_SIM_ENTITYLIST_HPP
