#include <dod/dod.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t count = 100'000;

struct DodPlayer : dod::Object<DodPlayer, count> {
    DOD_PROPERTY(int, health);
    DOD_PROPERTY(float, speed);
    DOD_PROPERTY(int, age);
};

struct AosPlayer {
    int health{};
    float speed{};
    int age{};
};

template <class Function>
double measure_ms(Function&& function) {
    const auto begin = std::chrono::steady_clock::now();
    function();
    const auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

void report(std::string_view name, double milliseconds) {
    std::cout << name << ": " << milliseconds << " ms\n";
}

} // namespace

int main() {
    std::vector<AosPlayer> aos;
    aos.reserve(count);
    std::vector<std::unique_ptr<DodPlayer>> soa;
    soa.reserve(count);

    report("AoS creation", measure_ms([&] {
               for (std::size_t i = 0; i < count; ++i) {
                   aos.push_back({static_cast<int>(i), 0.0f, 0});
               }
           }));
    report("DOD creation", measure_ms([&] {
               for (std::size_t i = 0; i < count; ++i) {
                   soa.push_back(std::make_unique<DodPlayer>());
                   soa.back()->health = static_cast<int>(i);
               }
           }));

    std::int64_t sink = 0;
    report("AoS sequential read", measure_ms([&] {
               for (const auto& player : aos) sink += player.health;
           }));
    report("proxy sequential read", measure_ms([&] {
               for (const auto& player : soa) sink += player->health.get();
           }));
    report("SoA span sequential read", measure_ms([&] {
               for (int value : DodPlayer::view<int, DodPlayer::health_tag>())
                   sink += value;
           }));

    report("AoS sequential write", measure_ms([&] {
               for (auto& player : aos) ++player.health;
           }));
    report("proxy sequential write", measure_ms([&] {
               for (auto& player : soa) ++player->health;
           }));
    report("SoA span sequential write", measure_ms([&] {
               for (int& value : DodPlayer::view<int, DodPlayer::health_tag>())
                   ++value;
           }));

    std::vector<std::size_t> order(count);
    std::iota(order.begin(), order.end(), 0);
    std::mt19937 random(42);
    std::shuffle(order.begin(), order.end(), random);
    report("AoS random read", measure_ms([&] {
               for (auto index : order) sink += aos[index].health;
           }));
    report("proxy random entity read", measure_ms([&] {
               for (auto index : order) sink += soa[index]->health.get();
           }));

    auto aos_for_deletion = aos;
    report("AoS swap-pop deletion", measure_ms([&] {
               while (!aos_for_deletion.empty()) {
                   const auto middle = aos_for_deletion.size() / 2;
                   aos_for_deletion[middle] = aos_for_deletion.back();
                   aos_for_deletion.pop_back();
               }
           }));
    report("DOD swap-pop deletion", measure_ms([&] {
               for (auto index : order) soa[index].reset();
           }));
    std::cout << "checksum: " << sink << '\n';
}
