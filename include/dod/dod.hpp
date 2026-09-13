#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <tuple>
#include <typeindex>
#include <typeinfo>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef DOD_ENABLE_HANDLE_CHECKS
#ifdef NDEBUG
#define DOD_ENABLE_HANDLE_CHECKS 0
#else
#define DOD_ENABLE_HANDLE_CHECKS 1
#endif
#endif

#if DOD_ENABLE_HANDLE_CHECKS != 0 && DOD_ENABLE_HANDLE_CHECKS != 1
#error "DOD_ENABLE_HANDLE_CHECKS must be 0 or 1"
#endif

namespace dod {

inline constexpr std::size_t default_capacity = 100'000;

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

template <class T, std::size_t Capacity,
          class Allocator = std::allocator<std::byte>>
class Column final : public IColumn {
    static_assert(std::default_initializable<T>,
                  "DOD property types must be default constructible");
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "DOD property types must be nothrow move constructible so "
                  "RAII destruction cannot leave columns inconsistent");
    static_assert(std::is_nothrow_destructible_v<T>,
                  "DOD property destructors must be noexcept");

public:
    using allocator_type = typename std::allocator_traits<Allocator>::template
        rebind_alloc<T>;
    using allocator_traits = std::allocator_traits<allocator_type>;
    using allocation_pointer = typename allocator_traits::pointer;

    explicit Column(std::size_t initial_size = 0,
                    const Allocator& allocator = Allocator{})
        : allocator_(allocator) {
        if (initial_size > Capacity) {
            throw std::length_error("initial column size exceeds capacity");
        }
        if constexpr (Capacity != 0) {
            allocation_ = allocator_traits::allocate(allocator_, Capacity);
            data_ = std::to_address(allocation_);
            allocated_ = true;
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
            if (allocated_) {
                allocator_traits::deallocate(allocator_, allocation_, Capacity);
                allocated_ = false;
                data_ = nullptr;
            }
        }
    }

    allocator_type allocator_;
    allocation_pointer allocation_{};
    T* data_{};
    std::size_t size_{};
    bool allocated_{};
};

template <class Owner, std::size_t Capacity,
          class Allocator = std::allocator<std::byte>>
class Registry {
    static_assert(Capacity <= EntityHandle::invalid_slot,
                  "capacity does not fit in EntityHandle::slot");
    static_assert(std::default_initializable<Allocator>,
                  "the DOD column allocator must be default constructible");

public:
    static Registry& instance() {
        static Registry registry;
        return registry;
    }

    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    [[nodiscard]] EntityHandle create() {
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
        if (!is_alive_impl(handle)) {
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
    Column<T, Capacity, Allocator>& column() {
        // One cache per Owner/Capacity/Tag/T. Columns live until registry
        // shutdown and never move.
        static Column<T, Capacity, Allocator>* cached = nullptr;
        if (cached != nullptr) {
            auto* result = cached;
            return *result;
        }

        const std::type_index key(typeid(Tag));
        if (const auto found = column_indices_.find(key);
            found != column_indices_.end()) {
            auto& existing = *columns_[found->second];
            if (existing.value_type() != typeid(T)) {
                throw std::logic_error(
                    "the property tag is already registered with another type");
            }
            auto* result = static_cast<Column<T, Capacity, Allocator>*>(&existing);
            cached = result;
            return *result;
        }

        auto created = std::make_unique<Column<T, Capacity, Allocator>>(
            dense_to_sparse_.size(), allocator_);
        auto* result = created.get();
        columns_.push_back(std::move(created));
        try {
            column_indices_.emplace(key, columns_.size() - 1);
        } catch (...) {
            columns_.pop_back();
            throw;
        }
        cached = result;
        return *result;
    }

    [[nodiscard]] std::size_t dense_index(EntityHandle handle) const {
#if DOD_ENABLE_HANDLE_CHECKS
        if (!is_alive_impl(handle)) {
            throw std::logic_error("access through a stale or moved-from handle");
        }
#endif
        return slots_[handle.slot].dense;
    }

    [[nodiscard]] bool is_alive(EntityHandle handle) const noexcept {
        return is_alive_impl(handle);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return dense_to_sparse_.size();
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

    struct Slot {
        std::uint32_t dense{npos};
        std::uint32_t generation{};
    };

    Registry() : slots_(Capacity) {
        dense_to_sparse_.reserve(Capacity);
        free_slots_.reserve(Capacity);
    }

    [[nodiscard]] bool is_alive_impl(EntityHandle handle) const noexcept {
        return handle.valid() && handle.slot < Capacity &&
               slots_[handle.slot].dense != npos &&
               slots_[handle.slot].generation == handle.generation;
    }

    // Keep the hash table on the cold schema path; structural loops traverse
    // the contiguous pointer list. Dense index and generation share a slot.
    std::unordered_map<std::type_index, std::size_t> column_indices_;
    std::vector<std::unique_ptr<IColumn>> columns_;
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> dense_to_sparse_;
    std::vector<std::uint32_t> free_slots_;
    std::uint32_t next_slot_{};
    [[no_unique_address]] Allocator allocator_{};
};

template <class Owner, class T, class Tag>
class Property {
public:
    explicit Property(EntityHandle handle)
        : handle_(handle) {
        if (column_ == nullptr) {
            column_ = &registry().template column<Tag, T>();
        }
    }

    Property(const Property&) = delete;

    Property(Property&& other) noexcept
        : handle_(std::exchange(other.handle_, {})) {}

    Property& operator=(const Property& other)
        requires requires(T& target, const T& value) { target = value; }
    {
        return set(other.const_ref());
    }

    Property& operator=(Property&& other) noexcept {
        if (this != &other) {
            handle_ = std::exchange(other.handle_, {});
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
        return (*column_)[registry().dense_index(handle_)];
    }

    [[nodiscard]] const T& const_ref() const {
        verify_proxy();
        return (*column_)[registry().dense_index(handle_)];
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
#if DOD_ENABLE_HANDLE_CHECKS
        if (!handle_.valid()) {
            throw std::logic_error("access through a moved-from property");
        }
#endif
    }

    using allocator_type = typename Owner::dod_allocator_type;
    using registry_type = Registry<Owner, Owner::dod_capacity, allocator_type>;
    using column_type = Column<T, Owner::dod_capacity, allocator_type>;

    static registry_type& registry() { return registry_type::instance(); }

    EntityHandle handle_;
    inline static column_type* column_{};
};

namespace detail {

template <class>
struct property_member_traits;

template <class Owner, class T, class Tag>
struct property_member_traits<Property<Owner, T, Tag> Owner::*> {
    using owner_type = Owner;
    using value_type = T;
    using tag_type = Tag;
};

template <class MemberPointer>
concept property_member_pointer = requires {
    typename property_member_traits<MemberPointer>::owner_type;
    typename property_member_traits<MemberPointer>::value_type;
    typename property_member_traits<MemberPointer>::tag_type;
};

} // namespace detail

template <class Derived, std::size_t Capacity = default_capacity,
          class Allocator = std::allocator<std::byte>>
class Object {
public:
    using dod_owner_type = Derived;
    using dod_allocator_type = Allocator;
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

    [[nodiscard]] EntityHandle dod_handle() const noexcept { return handle_; }

    template <auto Member>
        requires detail::property_member_pointer<decltype(Member)>
    [[nodiscard]] static auto view() {
        using traits = detail::property_member_traits<decltype(Member)>;
        static_assert(std::same_as<typename traits::owner_type, Derived>,
                      "the property must belong to this object type");
        return registry().template view<typename traits::tag_type,
                                        typename traits::value_type>();
    }

    template <auto... Members, class Function>
        requires(sizeof...(Members) > 0 &&
                 (detail::property_member_pointer<decltype(Members)> && ...))
    static void each(Function&& function) {
        static_assert(
            (std::same_as<typename detail::property_member_traits<
                              decltype(Members)>::owner_type,
                          Derived> && ...),
            "every property must belong to this object type");

        auto views = std::tuple{view<Members>()...};
        auto&& callable = function;
        const auto count = std::get<0>(views).size();
        for (std::size_t index = 0; index < count; ++index) {
            invoke_row(callable, views, index,
                       std::make_index_sequence<sizeof...(Members)>{});
        }
    }

    [[nodiscard]] static std::size_t size() noexcept { return registry().size(); }

    [[nodiscard]] static bool is_alive(EntityHandle handle) noexcept {
        return registry().is_alive(handle);
    }

protected:
    ~Object() {
        if (handle_.valid()) {
            registry().destroy(handle_);
        }
    }

private:
    template <class Function, class Views, std::size_t... Indices>
    static void invoke_row(Function& function, Views& views, std::size_t row,
                           std::index_sequence<Indices...>) {
        std::invoke(function, std::get<Indices>(views)[row]...);
    }

    static Registry<Derived, Capacity, Allocator>& registry() {
        return Registry<Derived, Capacity, Allocator>::instance();
    }

    EntityHandle handle_{};
};

} // namespace dod

#define DOD_PROPERTY(Type, Name)                                             \
    ::dod::Property<dod_owner_type, Type, decltype([] {})> Name{              \
        this->dod_handle()                                                    \
    }
