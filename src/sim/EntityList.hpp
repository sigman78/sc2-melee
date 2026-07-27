// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_ENTITYLIST_HPP
#define UQM2_SIM_ENTITYLIST_HPP

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#ifndef NDEBUG
#include <cstdio>
#endif

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

// A borrowed pointer to a live entity, checked in debug builds: removal can
// leave it pointing at a silently-wrong default T rather than UB. No
// implicit conversion to T* -- opt out explicitly with `.raw()`.
template <class T, class List>
class EntityRef
{
public:
	EntityRef() = default;

	EntityRef(T *ptr, [[maybe_unused]] const List *list,
			[[maybe_unused]] EntityId id) noexcept
		: ptr_(ptr)
#ifndef NDEBUG
		, list_(list)
		, id_(id)
		, epoch_(list != nullptr ? list->epoch() : 0)
#endif
	{
	}

	[[nodiscard]] T *
	operator->() const noexcept
	{
		check();
		return ptr_;
	}

	[[nodiscard]] T &
	operator*() const noexcept
	{
		check();
		return *ptr_;
	}

	[[nodiscard]] explicit operator bool() const noexcept
	{
		return ptr_ != nullptr;
	}

	[[nodiscard]] friend bool
	operator==(const EntityRef &r, std::nullptr_t) noexcept
	{
		return r.ptr_ == nullptr;
	}

	// The escape hatch, spelled out. Unchecked from here on.
	[[nodiscard]] T *
	raw() const noexcept
	{
		return ptr_;
	}

private:
	void
	check() const noexcept
	{
#ifndef NDEBUG
		if (ptr_ == nullptr || list_ == nullptr)
			return;  // a null ref is the caller's to test for, not ours
		if (list_->alive(id_))
			return;

		// The epoch delta is the useful half: it says how many list mutations
		// happened between the fetch and this dereference, which is what
		// points at the call that did it.
		std::fprintf(stderr,
				"entity %u (generation %u) was removed while a reference to "
				"it was held; the list changed %llu time(s) since it was "
				"fetched. Re-fetch through the EntityId after any call that "
				"can mutate the list.\n",
				id_.index, id_.generation,
				static_cast<unsigned long long>(list_->epoch() - epoch_));
		assert(false && "dereferenced a reference to a removed entity");
#endif
	}

	T *ptr_ = nullptr;
#ifndef NDEBUG
	const List *list_ = nullptr;
	EntityId id_;
	std::uint64_t epoch_ = 0;
#endif
};

// An ordered list of entities over a slot arena -- see design-notes D8.
template <class T>
class EntityList
{
public:
	[[nodiscard]] std::size_t size() const noexcept { return count_; }
	[[nodiscard]] bool empty() const noexcept { return count_ == 0; }

	[[nodiscard]] EntityId front() const noexcept { return head_; }
	[[nodiscard]] EntityId back() const noexcept { return tail_; }

	[[nodiscard]] EntityRef<T, EntityList> get(EntityId id) noexcept
	{
		return {alive(id) ? &slot(id.index).value : nullptr, this, id};
	}
	[[nodiscard]] EntityRef<const T, EntityList> get(EntityId id) const noexcept
	{
		return {alive(id) ? &slot(id.index).value : nullptr, this, id};
	}

	// How many times the list has been structurally changed. Only used by
	// EntityRef's diagnostic, but cheap enough to keep in every build.
	[[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }

	[[nodiscard]] EntityId next(EntityId id) const noexcept
	{
		assert(alive(id) && "next() of a dead entity");
		return slot(id.index).next;
	}
	[[nodiscard]] EntityId prev(EntityId id) const noexcept
	{
		assert(alive(id) && "prev() of a dead entity");
		return slot(id.index).prev;
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

		Slot &s = slot(id.index);
		if (s.prev.valid())
			slot(s.prev.index).next = s.next;
		else
			head_ = s.next;

		if (s.next.valid())
			slot(s.next.index).prev = s.prev;
		else
			tail_ = s.prev;

		s.value = T{};
		s.prev = kNoEntity;
		s.next = kNoEntity;
		// Bumping the generation is what turns a surviving handle into a
		// detectable mistake instead of a read of the next tenant.
		++s.generation;
		s.live = false;
		free_.push_back(id.index);
		--count_;
		++epoch_;
	}

	void
	clear() noexcept
	{
		for (std::uint32_t i = 0; i < used_; ++i)
		{
			Slot &s = slot(i);
			if (s.live)
			{
				s.value = T{};
				s.live = false;
				++s.generation;
				free_.push_back(i);
			}
		}
		head_ = kNoEntity;
		tail_ = kNoEntity;
		count_ = 0;
		++epoch_;
	}

	// Reserving up front is the point of the arena: a battle's entity count
	// is bounded, so steady-state stepping should allocate nothing.
	void
	reserve(std::size_t n)
	{
		while (capacity_ < n)
			grow();
		free_.reserve(n);
	}

	[[nodiscard]] bool
	alive(EntityId id) const noexcept
	{
		if (!id.valid() || id.index >= used_)
			return false;
		const Slot &s = slot(id.index);
		return s.live && s.generation == id.generation;
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

	// Chunk size is a tradeoff between a wasted tail chunk and how often we
	// allocate. A melee is a few dozen entities, so one chunk is the whole
	// battle and the free list serves every spawn after that.
	static constexpr std::uint32_t kChunkShift = 6;
	static constexpr std::uint32_t kChunkSize = 1u << kChunkShift;

	[[nodiscard]] Slot &
	slot(std::uint32_t i) noexcept
	{
		return chunks_[i >> kChunkShift][i & (kChunkSize - 1)];
	}
	[[nodiscard]] const Slot &
	slot(std::uint32_t i) const noexcept
	{
		return chunks_[i >> kChunkShift][i & (kChunkSize - 1)];
	}

	void
	grow()
	{
		chunks_.push_back(std::make_unique<Slot[]>(kChunkSize));
		capacity_ += kChunkSize;
	}

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
			if (used_ == capacity_)
				grow();
			index = used_++;
		}

		Slot &s = slot(index);
		s.value = std::move(value);
		s.live = true;
		++count_;
		++epoch_;
		return EntityId{index, s.generation};
	}

	void
	linkAfter(EntityId after, EntityId id)
	{
		Slot &s = slot(id.index);
		if (!after.valid())
		{
			s.prev = kNoEntity;
			s.next = head_;
			if (head_.valid())
				slot(head_.index).prev = id;
			head_ = id;
			if (!tail_.valid())
				tail_ = id;
			return;
		}

		Slot &prevSlot = slot(after.index);
		s.prev = after;
		s.next = prevSlot.next;
		if (prevSlot.next.valid())
			slot(prevSlot.next.index).prev = id;
		else
			tail_ = id;
		prevSlot.next = id;
	}

	std::vector<std::unique_ptr<Slot[]>> chunks_;
	std::vector<std::uint32_t> free_;
	// Slots ever handed out, and slots constructed. Everything at or past
	// `used_` has never been touched, so `clear` need not walk it.
	std::uint32_t used_ = 0;
	std::uint32_t capacity_ = 0;
	EntityId head_ = kNoEntity;
	EntityId tail_ = kNoEntity;
	std::size_t count_ = 0;
	std::uint64_t epoch_ = 0;
};

}  // namespace uqm::sim

#endif  // UQM2_SIM_ENTITYLIST_HPP
