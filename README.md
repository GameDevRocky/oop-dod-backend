# oop-dod-backend

A C++20 header-only library that presents ordinary, move-only objects while
storing each declared property in its own contiguous Structure-of-Arrays
column.

```cpp
#include <dod/dod.hpp>

class Player : public dod::Object<Player, 100000> {
public:
    DOD_PROPERTY(int, health);
    DOD_PROPERTY(float, speed);
    DOD_PROPERTY(int, age);

    void take_damage(int amount) { health -= amount; }
};

Player player;
player.health = 100;
player.take_damage(20);

for (int& health : Player::view<int, Player::health_tag>()) {
    health += 10;
}
```

No manager, storage class, property number, or pool resizing is required.
Properties with the same value type remain separate because their nested tag
types identify their columns.

## CMake usage

```cmake
add_subdirectory(oop-dod-backend)
target_link_libraries(your_target PRIVATE dod::dod)
```

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

## Thread-safety contract

Column registration and all structural operations are serialized per object
type. Concurrent reads are permitted while no structural change occurs.
Callers must prevent data races for writes, including writes to different
values unless their access pattern is independently synchronized. Access to
the same value needs synchronization (or an atomic property type). Entity
creation or destruction must not overlap an unprotected batch view.
