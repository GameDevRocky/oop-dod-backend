# oop-dod-backend

A C++20 header-only library that presents ordinary, move-only objects while
storing each declared property in its own contiguous Structure-of-Arrays
column.

```cpp
#include <dod/dod.hpp>

class Player : public dod::Object<Player> {
public:
    DOD_PROPERTY(int, health);
    DOD_PROPERTY(float, speed);
    DOD_PROPERTY(int, age);

    void take_damage(int amount) { health -= amount; }
};

Player player;
player.health = 100;
player.take_damage(20);

for (int& health : Player::view<&Player::health>()) {
    health += 10;
}
```

No manager, storage class, property number, or pool resizing is required.
Properties with the same value type remain separate because each property has
a unique internal tag type.

Pass a property member to `view` to infer both its value type and internal
column tag:

```cpp
for (int& age : Player::view<&Player::age>()) {
    ++age;
}
```

## CMake usage

```cmake
add_subdirectory(oop-dod-backend)
target_link_libraries(your_target PRIVATE dod::dod)
```

The default capacity is 100,000 objects. Override it only when needed:

```cpp
class Projectile : public dod::Object<Projectile, 2'000'000> {
public:
    DOD_PROPERTY(float, lifetime);
};
```

## Multiple-property iteration

Use `each` when a system updates several columns together:

```cpp
Player::each<&Player::speed, &Player::age>(
    [](float& speed, int& age) {
        speed += 0.25f;
        ++age;
    });
```

The callback receives references in the same order as the listed properties.
It may throw; changes made to earlier rows remain applied. Do not create or
delete `Player` objects from inside the callback because that invalidates the
column views driving the iteration.

## Handle checks

Handle validation is enabled in Debug configurations and disabled elsewhere by
default. Override it while configuring a CMake consumer:

```sh
cmake -S . -B build -DDOD_ENABLE_HANDLE_CHECKS=ON
```

Accepted values are `AUTO`, `ON`, and `OFF`. Projects that include the header
without CMake can define `DOD_ENABLE_HANDLE_CHECKS` to `0` or `1` before the
header. Disabled checks make stale and moved-from property access undefined.

## Custom column allocator

The third `Object` template argument is a byte allocator. It is rebound to each
property type using `std::allocator_traits`:

```cpp
template<class T>
using GameAllocator = /* allocator type */;

class Player : public dod::Object<
    Player,
    1'000'000,
    GameAllocator<std::byte>> {
public:
    DOD_PROPERTY(int, age);
};
```

The allocator must be default constructible and support allocator rebinding.
It controls property-column allocations; registry bookkeeping continues to use
the standard allocator. A default-constructed
`std::pmr::polymorphic_allocator<std::byte>` can use the process's current PMR
default resource when the registry is first created.

## Semantics and constraints

- Objects are non-copyable and `noexcept` movable. A move transfers the stable
  generational entity handle and invalidates every moved-from proxy.
- Deletion uses swap-and-pop across all registered columns. Handles identify a
  sparse slot plus generation, so other live objects remain valid.
- A property type must be default constructible, nothrow move constructible,
  and nothrow destructible. This lets automatic RAII destruction preserve all
  columns without a potentially throwing partial relocation. Types such as
  `std::string` are supported.
- `ref()` and `const_ref()` expose temporary references. A reference, pointer
  from `operator->`, or span from `view()` is valid only until the next entity
  creation, destruction, or other structural storage mutation.
- Capacity overflow throws `std::length_error`, including release builds.
- Each property proxy stores only its entity handle. Its typed column pointer
  is shared statically by all proxies for that property.
- The library is intentionally single-threaded and performs no internal
  synchronization.
