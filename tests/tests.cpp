#include <dod/dod.hpp>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

int failures = 0;

#define CHECK(expression)                                                    \
    do {                                                                     \
        if (!(expression)) {                                                 \
            std::cerr << __FILE__ << ':' << __LINE__                         \
                      << ": CHECK failed: " #expression << '\n';            \
            ++failures;                                                      \
        }                                                                    \
    } while (false)

template <class Exception, class Function>
void check_throws(Function&& function, int line) {
    try {
        function();
        std::cerr << __FILE__ << ':' << line << ": expected exception\n";
        ++failures;
    } catch (const Exception&) {
    } catch (...) {
        std::cerr << __FILE__ << ':' << line << ": wrong exception type\n";
        ++failures;
    }
}

#define CHECK_THROWS_AS(expression, exception_type)                          \
    check_throws<exception_type>([&] { (void)(expression); }, __LINE__)

struct Player : dod::Object<Player, 32> {
    DOD_PROPERTY(int, health);
    DOD_PROPERTY(float, speed);
    DOD_PROPERTY(int, age);

    void take_damage(int amount) { health -= amount; }
};

struct Small : dod::Object<Small, 3> {
    DOD_PROPERTY(int, value);
};

struct TextObject : dod::Object<TextObject, 8> {
    DOD_PROPERTY(std::string, text);
};

struct SometimesThrows {
    inline static bool should_throw = false;

    SometimesThrows() {
        if (should_throw) {
            throw std::runtime_error("requested test failure");
        }
    }
    SometimesThrows(SometimesThrows&&) noexcept = default;
};

struct ThrowingObject : dod::Object<ThrowingObject, 4> {
    DOD_PROPERTY(SometimesThrows, value);
};

struct Position {
    float x{};
    float y{};
};

struct Structured : dod::Object<Structured, 4> {
    DOD_PROPERTY(Position, position);
};

struct alignas(128) OverAligned {
    std::uint64_t value{};
};

struct AlignedObject : dod::Object<AlignedObject, 4> {
    DOD_PROPERTY(OverAligned, data);
};

struct Tracked {
    inline static std::atomic<int> live = 0;
    int value{};

    Tracked() { ++live; }
    Tracked(Tracked&& other) noexcept : value(other.value) { ++live; }
    Tracked& operator=(Tracked&&) = delete;
    ~Tracked() noexcept { --live; }
};

struct TrackedObject : dod::Object<TrackedObject, 8> {
    DOD_PROPERTY(Tracked, tracked);
};

void scalar_and_distinct_columns() {
    Player player;
    player.health = 100;
    player.speed = 5.5f;
    player.age = 41;
    player.take_damage(20);
    player.health += 2;
    ++player.health;
    player.age--;

    CHECK(static_cast<int>(player.health) == 83);
    CHECK(static_cast<float>(player.speed) == 5.5f);
    CHECK(static_cast<int>(player.age) == 40);
    CHECK((Player::view<int, Player::health_tag>().data() !=
           Player::view<int, Player::age_tag>().data()));
    CHECK_THROWS_AS((Player::view<float, Player::health_tag>()), std::logic_error);
}

void capacity_and_raii() {
    CHECK(Small::size() == 0);
    {
        Small a;
        Small b;
        Small c;
        CHECK(Small::size() == 3);
        CHECK_THROWS_AS(Small{}, std::length_error);
    }
    CHECK(Small::size() == 0);
}

void swap_pop_and_stable_handles() {
    auto first = std::make_unique<Player>();
    auto middle = std::make_unique<Player>();
    auto last = std::make_unique<Player>();
    first->health = 10;
    middle->health = 20;
    last->health = 30;
    const auto stale = first->dod_handle();
    const auto middle_handle = middle->dod_handle();
    const auto last_handle = last->dod_handle();

    first.reset();
    CHECK(!Player::is_alive(stale));
    CHECK(Player::is_alive(middle_handle));
    CHECK(Player::is_alive(last_handle));
    CHECK(static_cast<int>(middle->health) == 20);
    CHECK(static_cast<int>(last->health) == 30);

    middle.reset();
    CHECK(static_cast<int>(last->health) == 30);
    last.reset();
    CHECK(Player::size() == 0);

    Player reused;
    const auto replacement = reused.dod_handle();
    CHECK(replacement.slot == last_handle.slot);
    CHECK(replacement.generation == last_handle.generation + 1);
    dod::Property<Player, int, Player::health_tag> stale_property(stale);
    CHECK_THROWS_AS(stale_property.get(), std::logic_error);

    std::vector<std::unique_ptr<Player>> group;
    for (int i = 0; i < 5; ++i) {
        group.push_back(std::make_unique<Player>());
        group.back()->health = i * 10;
    }
    const auto first_group_handle = group.front()->dod_handle();
    const auto middle_group_handle = group[2]->dod_handle();
    const auto last_group_handle = group.back()->dod_handle();
    group[2].reset();
    CHECK(!Player::is_alive(middle_group_handle));
    CHECK(Player::is_alive(first_group_handle));
    CHECK(Player::is_alive(last_group_handle));
    CHECK(static_cast<int>(group.front()->health) == 0);
    CHECK(static_cast<int>(group.back()->health) == 40);
}

void move_semantics() {
    Player source;
    source.health = 77;
    const auto handle = source.dod_handle();
    Player destination(std::move(source));
    CHECK(!source.dod_handle().valid());
    CHECK(Player::is_alive(handle));
    CHECK(static_cast<int>(destination.health) == 77);
    CHECK_THROWS_AS(source.health.get(), std::logic_error);

    Player assigned;
    assigned.health = 1;
    assigned = std::move(destination);
    CHECK(!destination.dod_handle().valid());
    CHECK(static_cast<int>(assigned.health) == 77);
    CHECK(Player::size() == 1);
}

void non_trivial_structured_and_alignment() {
    {
        TextObject object;
        object.text = "hello SoA";
        CHECK(object.text.const_ref() == "hello SoA");
    }
    {
        Structured object;
        object.position->x = 10.0f;
        CHECK(object.position.const_ref().x == 10.0f);
    }
    {
        AlignedObject object;
        auto* address = std::addressof(object.data.ref());
        CHECK(reinterpret_cast<std::uintptr_t>(address) % alignof(OverAligned) == 0);
    }
    CHECK(Tracked::live == 0);
    {
        TrackedObject a;
        TrackedObject b;
        CHECK(Tracked::live == 2);
        TrackedObject c(std::move(a));
        CHECK(Tracked::live == 2);
        b = std::move(c);
        CHECK(Tracked::live == 1);
    }
    CHECK(Tracked::live == 0);
}

void exception_rollback() {
    CHECK(ThrowingObject::size() == 0);
    {
        ThrowingObject first;
        CHECK(ThrowingObject::size() == 1);
        SometimesThrows::should_throw = true;
        CHECK_THROWS_AS(ThrowingObject{}, std::runtime_error);
        CHECK(ThrowingObject::size() == 1);
        SometimesThrows::should_throw = false;
    }
    CHECK(ThrowingObject::size() == 0);
}

struct Concurrent : dod::Object<Concurrent, 16> {
    DOD_PROPERTY(int, first);
    DOD_PROPERTY(std::string, second);
    DOD_PROPERTY(int, third);
};

void concurrent_schema_and_construction() {
    constexpr int count = 8;
    std::barrier constructed(count);
    std::barrier written(count);
    std::vector<std::thread> threads;
    threads.reserve(count);
    for (int i = 0; i < count; ++i) {
        threads.emplace_back([i, &constructed, &written] {
            Concurrent object;
            constructed.arrive_and_wait();
            object.first = i;
            object.second = std::to_string(i);
            object.third = i * 2;
            written.arrive_and_wait();
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK(Concurrent::size() == 0);
}

struct Batch : dod::Object<Batch, 8> {
    DOD_PROPERTY(int, value);
};

void batch_view() {
    Batch a;
    Batch b;
    Batch c;
    a.value = 2;
    b.value = 4;
    c.value = 6;
    auto values = Batch::view<int, Batch::value_tag>();
    CHECK(values.size() == 3);
    for (int& value : values) {
        value += 10;
    }
    CHECK(static_cast<int>(a.value) == 12);
    CHECK(static_cast<int>(b.value) == 14);
    CHECK(static_cast<int>(c.value) == 16);
}

} // namespace

int main() {
    static_assert(!std::copy_constructible<Player>);
    static_assert(!std::is_copy_assignable_v<Player>);
    static_assert(std::is_nothrow_move_constructible_v<Player>);
    static_assert(std::is_nothrow_move_assignable_v<Player>);

    scalar_and_distinct_columns();
    capacity_and_raii();
    swap_pop_and_stable_handles();
    move_semantics();
    non_trivial_structured_and_alignment();
    exception_rollback();
    concurrent_schema_and_construction();
    batch_view();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "all tests passed\n";
}
