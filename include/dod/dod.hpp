#pragma once

#include <atomic>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <stdexcept>
#include <typeindex>
#include <typeinfo>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dod {

struct EntityHandle {
    std::uint32_t slot{invalid_slot};
    std::uint32_t generation{};

    static constexpr std::uint32_t invalid_slot =
        std::numeric_limits<std::uint32_t>::max();

    [[nodiscard]] constexpr bool valid() const noexcept {
        return slot != invalid_slot;
    }

    friend constexpr bool operator==(EntityHandle, EntityHandle) = default;
};

class IColumn {
public:
    virtual ~IColumn() = default;
    virtual void append_default() = 0;
    virtual void swap_and_pop(std::size_t removed, std::size_t last) noexcept = 0;
    virtual void rollback_last() noexcept = 0;
    [[nodiscard]] virtual const std::type_info& value_type() const noexcept = 0;
    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
};

template <class T, std::size_t Capacity>
class Column final : public IColumn {
    static_assert(std::default_initializable<T>,
                  "DOD property types must be default constructible");
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "DOD property types must be nothrow move constructible so "
                  "RAII destruction cannot leave columns inconsistent");
    static_assert(std::is_nothrow_destructible_v<T>,
                  "DOD property destructors must be noexcept");

public:
    explicit Column(std::size_t initial_size = 0) {
        if (initial_size > Capacity) {
            throw std::length_error("initial column size exceeds capacity");
        }
        if constexpr (Capacity != 0) {
            data_ = allocator_.allocate(Capacity);
        }
        try {
            while (size_ < initial_size) {
                std::construct_at(data_ + size_);
                ++size_;
            }
        } catch (...) {
            clear();
            deallocate();
            throw;
        }
    }

    Column(const Column&) = delete;
    Column& operator=(const Column&) = delete;

    ~Column() override {
        clear();
        deallocate();
    }

    void append_default() override {
        if (size_ == Capacity) {
            throw std::length_error("column capacity exceeded");
        }
        std::construct_at(data_ + size_);
        ++size_;
    }

    void swap_and_pop(std::size_t removed, std::size_t last) noexcept override {
        assert(size_ != 0 && last == size_ - 1 && removed <= last);
        if (removed != last) {
            std::destroy_at(data_ + removed);
            std::construct_at(data_ + removed, std::move(data_[last]));
        }
        std::destroy_at(data_ + last);
        --size_;
    }

    void rollback_last() noexcept override {
        assert(size_ != 0);
        std::destroy_at(data_ + (size_ - 1));
        --size_;
    }

    [[nodiscard]] const std::type_info& value_type() const noexcept override {
        return typeid(T);
    }

    [[nodiscard]] std::size_t size() const noexcept override { return size_; }
    [[nodiscard]] T* data() noexcept { return data_; }
    [[nodiscard]] const T* data() const noexcept { return data_; }

    [[nodiscard]] T& operator[](std::size_t index) noexcept {
        assert(index < size_);
        return data_[index];
    }

    [[nodiscard]] const T& operator[](std::size_t index) const noexcept {
        assert(index < size_);
        return data_[index];
    }

private:
    void clear() noexcept {
        while (size_ != 0) {
            --size_;
            std::destroy_at(data_ + size_);
        }
    }

    void deallocate() noexcept {
        if constexpr (Capacity != 0) {
            if (data_ != nullptr) {
                allocator_.deallocate(data_, Capacity);
                data_ = nullptr;
            }
        }
    }

    std::allocator<T> allocator_;
    T* data_{};
    std::size_t size_{};
};

template <class Owner, std::size_t Capacity>
class Registry {
    static_assert(Capacity <= EntityHandle::invalid_slot,
                  "capacity does not fit in EntityHandle::slot");

public:
    static Registry& instance() {
        static Registry registry;
        return registry;
    }

    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    [[nodiscard]] EntityHandle create() {
        auto lock = lock_structure();
        if (dense_to_sparse_.size() == Capacity) {
            throw std::length_error("DOD registry capacity exceeded");
        }

        const auto slot = free_slots_.empty()
                              ? next_slot_
                              : free_slots_.back();
        std::size_t appended = 0;
        try {
            for (auto& column : columns_) {
                column->append_default();
                ++appended;
            }
        } catch (...) {
            while (appended != 0) {
                columns_[--appended]->rollback_last();
            }
            throw;
        }

        if (!free_slots_.empty()) {
            free_slots_.pop_back();
        } else {
            ++next_slot_;
        }
        const auto dense = dense_to_sparse_.size();
        dense_to_sparse_.push_back(slot);
        slots_[slot].dense = static_cast<std::uint32_t>(dense);
        return {slot, slots_[slot].generation};
    }

    void destroy(EntityHandle handle) noexcept {
        auto lock = lock_structure();
        if (!is_alive_unlocked(handle)) {
#ifndef NDEBUG
            assert(false && "attempted to destroy a stale DOD handle");
#endif
            return;
        }

        const auto removed = slots_[handle.slot].dense;
        const auto last = dense_to_sparse_.size() - 1;
        if (removed != last) {
            for (auto& column : columns_) {
                column->swap_and_pop(removed, last);
            }
            const auto moved_slot = dense_to_sparse_[last];
            dense_to_sparse_[removed] = moved_slot;
            slots_[moved_slot].dense = removed;
        } else {
            for (auto& column : columns_) {
                column->rollback_last();
            }
        }
        dense_to_sparse_.pop_back();
        slots_[handle.slot].dense = npos;
        ++slots_[handle.slot].generation;
        free_slots_.push_back(handle.slot);
    }

    template <class Tag, class T>
    Column<T, Capacity>& column() {
        // One cache per Owner/Capacity/Tag/T, safely published after complete
        // registration. Columns live until registry shutdown and never move.
        static std::atomic<Column<T, Capacity>*> cached{nullptr};
        if (auto* result = cached.load(std::memory_order_acquire)) {
            return *result;
        }

        auto lock = lock_structure();
        const std::type_index key(typeid(Tag));
        if (const auto found = column_indices_.find(key);
            found != column_indices_.end()) {
            auto& existing = *columns_[found->second];
            if (existing.value_type() != typeid(T)) {
                throw std::logic_error(
                    "the property tag is already registered with another type");
            }
            auto* result = static_cast<Column<T, Capacity>*>(&existing);
            cached.store(result, std::memory_order_release);
            return *result;
        }

        auto created = std::make_unique<Column<T, Capacity>>(
            dense_to_sparse_.size());
        auto* result = created.get();
        columns_.push_back(std::move(created));
        try {
            column_indices_.emplace(key, columns_.size() - 1);
        } catch (...) {
            columns_.pop_back();
            throw;
        }
        cached.store(result, std::memory_order_release);
        return *result;
    }

    [[nodiscard]] std::size_t dense_index(EntityHandle handle) const {
        // Property access deliberately takes no lock. The documented contract
        // forbids structural mutation concurrent with unprotected access.
        if (!is_alive_unlocked(handle)) {
            throw std::logic_error("access through a stale or moved-from handle");
        }
        return slots_[handle.slot].dense;
    }

    [[nodiscard]] bool is_alive(EntityHandle handle) const noexcept {
        auto lock = lock_structure();
        return is_alive_unlocked(handle);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        auto lock = lock_structure();
        return dense_to_sparse_.size();
    }

    template <class Function>
    decltype(auto) with_structural_batch(Function&& function) {
        auto lock = lock_structure();
        struct RestoreBatchState {
            Registry& registry;
            bool previous;
            ~RestoreBatchState() noexcept {
                batch_active_ = previous;
                if (!previous) {
                    registry.batch_in_progress_.store(false, std::memory_order_relaxed);
                }
            }
        } restore{*this, std::exchange(batch_active_, true)};
        batch_in_progress_.store(true, std::memory_order_relaxed);
        return std::forward<Function>(function)();
    }

    template <class Tag, class T>
    [[nodiscard]] std::span<T> view() {
        auto& values = column<Tag, T>();
        // Fixed-capacity columns never reallocate. The logical contents and
        // span length are nevertheless invalid after a structural mutation.
        return {values.data(), values.size()};
    }

private:
    static constexpr std::uint32_t npos = EntityHandle::invalid_slot;

    // Only the thread that owns the outer batch lock skips per-operation
    // locking. Other threads continue to acquire the same registry mutex.
    inline static thread_local bool batch_active_ = false;

    [[nodiscard]] std::unique_lock<std::mutex> lock_structure() const {
        std::unique_lock lock(mutex_, std::defer_lock);
        // Most calls are outside a batch. Avoid a TLS lookup on that path
        // (some toolchains implement TLS with a comparatively costly call).
        // This flag is only a hint: mutex ownership or this thread's batch
        // state, not the flag, determines whether it may mutate the registry.
        if (!batch_in_progress_.load(std::memory_order_relaxed) || !batch_active_) {
            lock.lock();
        }
        return lock;
    }

    struct Slot {
        std::uint32_t dense{npos};
        std::uint32_t generation{};
    };

    Registry() : slots_(Capacity) {
        dense_to_sparse_.reserve(Capacity);
        free_slots_.reserve(Capacity);
    }

    [[nodiscard]] bool is_alive_unlocked(EntityHandle handle) const noexcept {
        return handle.valid() && handle.slot < Capacity &&
               slots_[handle.slot].dense != npos &&
               slots_[handle.slot].generation == handle.generation;
    }

    mutable std::mutex mutex_;
    std::atomic<bool> batch_in_progress_{false};
    // Keep the hash table on the cold schema path; structural loops traverse
    // the contiguous pointer list. Dense index and generation share a slot.
    std::unordered_map<std::type_index, std::size_t> column_indices_;
    std::vector<std::unique_ptr<IColumn>> columns_;
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> dense_to_sparse_;
    std::vector<std::uint32_t> free_slots_;
    std::uint32_t next_slot_{};
};

template <class Owner, class T, class Tag>
class Property {
public:
    explicit Property(EntityHandle handle)
        : handle_(handle), column_(&Owner::template dod_column<Tag, T>()) {}

    Property(const Property&) = delete;

    Property(Property&& other) noexcept
        : handle_(std::exchange(other.handle_, {})),
          column_(std::exchange(other.column_, nullptr)) {}

    Property& operator=(const Property& other)
        requires requires(T& target, const T& value) { target = value; }
    {
        return set(other.const_ref());
    }

    Property& operator=(Property&& other) noexcept {
        if (this != &other) {
            handle_ = std::exchange(other.handle_, {});
            column_ = std::exchange(other.column_, nullptr);
        }
        return *this;
    }

    template <class U>
        requires(!std::same_as<std::remove_cvref_t<U>, Property> &&
                 requires(T& target, U&& value) {
                     target = std::forward<U>(value);
                 })
    Property& operator=(U&& value) {
        return set(std::forward<U>(value));
    }

    [[nodiscard]] T get() const
        requires std::copy_constructible<T>
    {
        return const_ref();
    }

    template <class U>
        requires requires(T& target, U&& candidate) {
            target = std::forward<U>(candidate);
        }
    Property& set(U&& value) {
        ref() = std::forward<U>(value);
        return *this;
    }

    [[nodiscard]] T& ref() {
        verify_proxy();
        return (*column_)[Owner::dod_dense_index(handle_)];
    }

    [[nodiscard]] const T& const_ref() const {
        verify_proxy();
        return (*column_)[Owner::dod_dense_index(handle_)];
    }

    operator T() const
        requires std::copy_constructible<T>
    {
        return get();
    }

    [[nodiscard]] T* operator->() { return std::addressof(ref()); }
    [[nodiscard]] const T* operator->() const {
        return std::addressof(const_ref());
    }

#define DOD_DETAIL_COMPOUND(OP)                                              \
    template <class U>                                                       \
        requires requires(T& value, U&& operand) {                           \
            value OP std::forward<U>(operand);                               \
        }                                                                    \
    Property& operator OP(U&& operand) {                                     \
        ref() OP std::forward<U>(operand);                                   \
        return *this;                                                        \
    }

    DOD_DETAIL_COMPOUND(+=)
    DOD_DETAIL_COMPOUND(-=)
    DOD_DETAIL_COMPOUND(*=)
    DOD_DETAIL_COMPOUND(/=)
    DOD_DETAIL_COMPOUND(%=)
    DOD_DETAIL_COMPOUND(&=)
    DOD_DETAIL_COMPOUND(|=)
    DOD_DETAIL_COMPOUND(^=)
    DOD_DETAIL_COMPOUND(<<=)
    DOD_DETAIL_COMPOUND(>>=)

#undef DOD_DETAIL_COMPOUND

    Property& operator++()
        requires requires(T& value) { ++value; }
    {
        ++ref();
        return *this;
    }

    T operator++(int)
        requires(std::copy_constructible<T> && requires(T& value) { value++; })
    {
        T previous = ref();
        ref()++;
        return previous;
    }

    Property& operator--()
        requires requires(T& value) { --value; }
    {
        --ref();
        return *this;
    }

    T operator--(int)
        requires(std::copy_constructible<T> && requires(T& value) { value--; })
    {
        T previous = ref();
        ref()--;
        return previous;
    }

private:
    void verify_proxy() const {
        if (!handle_.valid() || column_ == nullptr) {
            throw std::logic_error("access through a moved-from property");
        }
    }

    EntityHandle handle_;
    Column<T, Owner::dod_capacity>* column_{};
};

template <class Derived, std::size_t Capacity>
class Object {
public:
    using dod_owner_type = Derived;
    static constexpr std::size_t dod_capacity = Capacity;

    Object() : handle_(registry().create()) {}
    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;

    Object(Object&& other) noexcept
        : handle_(std::exchange(other.handle_, {})) {}

    Object& operator=(Object&& other) noexcept {
        if (this != &other) {
            if (handle_.valid()) {
                registry().destroy(handle_);
            }
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    ~Object() {
        if (handle_.valid()) {
            registry().destroy(handle_);
        }
    }

    [[nodiscard]] EntityHandle dod_handle() const noexcept { return handle_; }

    template <class T, class Tag>
    [[nodiscard]] static std::span<T> view() {
        return registry().template view<Tag, T>();
    }

    [[nodiscard]] static std::size_t size() noexcept { return registry().size(); }

    // Execute synchronous lifecycle operations under one registry lock.
    // Each entity retains its usual RAII, capacity, and rollback semantics.
    template <class Function>
    static decltype(auto) with_structural_batch(Function&& function) {
        return registry().with_structural_batch(std::forward<Function>(function));
    }

    [[nodiscard]] static bool is_alive(EntityHandle handle) noexcept {
        return registry().is_alive(handle);
    }

    template <class Tag, class T>
    static Column<T, Capacity>& dod_column() {
        return registry().template column<Tag, T>();
    }

    [[nodiscard]] static std::size_t dod_dense_index(EntityHandle handle) {
        return registry().dense_index(handle);
    }

private:
    static Registry<Derived, Capacity>& registry() {
        return Registry<Derived, Capacity>::instance();
    }

    EntityHandle handle_{};
};

} // namespace dod

#define DOD_PROPERTY(Type, Name)                                             \
    struct Name##_tag final {};                                              \
    ::dod::Property<dod_owner_type, Type, Name##_tag> Name{this->dod_handle()}
