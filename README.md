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

## Creating or deleting many objects

Repeated property construction reuses a cached column pointer; it does not
lock or search the schema again after that column has been registered. Entity
creation and deletion traverse a contiguous list of columns. Sparse slots use
32-bit dense indices alongside their generation counters.

For large creation or deletion loops, use `with_structural_batch` to hold the
registry mutex once for the whole callback:

```cpp
#include <vector>

std::vector<Player> players;
players.reserve(100000);

Player::with_structural_batch([&] {
    for (int i = 0; i < 100000; ++i) {
        players.emplace_back();
    }
});

// Update existing objects or their column views as usual.

Player::with_structural_batch([&] {
    players.clear();
});
```

This also works with `new Player`, `delete player`, and smart pointers inside
the callback. Every entity is still created and destroyed normally; the batch
amortizes locking. It does not eliminate per-object heap allocations if you
choose to use `new` or `make_unique`.

The callback runs synchronously on the calling thread. Nested batches for the
same object type are allowed, and the callback's return value is forwarded.
Other threads' structural operations for that type wait until the batch ends.
Do not wait inside the callback for another thread that needs this registry.
If acquiring batches for multiple object types, use a consistent lock order.
User-defined property constructors/destructors must not re-enter structural
operations on their own registry, including from within a batch.

If the callback throws, the lock is released and the exception propagates.
Completed operations remain completed; the batch is not a transaction. An
individual failed entity or column construction still rolls back normally.

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
type. Concurrent reads and writes to distinct values are allowed while no
structural change occurs, provided the property type itself does not introduce
shared mutable state. Concurrent access to the same value with at least one
writer requires caller synchronization. Property reads and writes, references,
and spans do not acquire a registry lock. Creation or destruction must not run
concurrently with unprotected access to any property or view of that type.
The structural batch helper does not make such unprotected access safe, and a
structural mutation still invalidates existing spans and property references
inside a batch.
