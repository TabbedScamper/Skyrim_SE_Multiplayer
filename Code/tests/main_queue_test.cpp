#include "../client/MainQueue.h"
#include <array>
#include <atomic>
#include <thread>
#include <vector>
#include <catch2/catch.hpp>

TEST_CASE("MainQueue holds a key's later records behind a held one and drains in order", "[main_queue]")
{
    MainQueue::Queue<4> queue;
    MainQueue::Record first{}, second{}, third{}, fourth{};
    first.OrderKey = 7; first.Kind = 1;
    second.OrderKey = 7; second.Kind = 2;
    third.OrderKey = 9; third.Kind = 3;
    fourth.OrderKey = 7; fourth.Kind = 4;
    REQUIRE((queue.Push(first) && queue.Push(second) && queue.Push(third) && queue.Push(fourth)));
    REQUIRE(!queue.Push(first));
    REQUIRE(queue.PoolFull() == 1);
    std::vector<uint16_t> applied;
    auto action = [&](const MainQueue::Record& record) {
        if (record.Kind == 1) return MainQueue::Decision::Hold;
        applied.push_back(record.Kind);
        return MainQueue::Decision::Apply;
    };
    REQUIRE(queue.Drain(4, action) == 1);
    REQUIRE((applied == std::vector<uint16_t>{3}));
    REQUIRE(queue.Outstanding() == 3);
    REQUIRE(queue.Drain(4, [&](const MainQueue::Record& record) {
        applied.push_back(record.Kind);
        return MainQueue::Decision::Apply;
    }) == 3);
    REQUIRE((applied == std::vector<uint16_t>{3, 1, 2, 4}));
    REQUIRE(queue.Outstanding() == 0);
}

TEST_CASE("MainQueue takes pushes from two threads without loss or duplicates", "[main_queue]")
{
    MainQueue::Queue<256> concurrent;
    std::atomic<int> failedPushes{};
    std::thread a([&] { for (uint16_t i = 0; i < 100; ++i) { MainQueue::Record r{}; r.Kind = i; if (!concurrent.Push(r)) ++failedPushes; } });
    std::thread b([&] { for (uint16_t i = 100; i < 200; ++i) { MainQueue::Record r{}; r.Kind = i; if (!concurrent.Push(r)) ++failedPushes; } });
    a.join();
    b.join();
    REQUIRE(failedPushes == 0);
    std::array<bool, 200> seen{};
    bool duplicate = false;
    REQUIRE(concurrent.Drain(256, [&](const MainQueue::Record& record) {
        if (record.Kind >= seen.size() || seen[record.Kind])
            duplicate = true;
        else
            seen[record.Kind] = true;
        return MainQueue::Decision::Apply;
    }) == 200);
    REQUIRE(!duplicate);
    for (auto value : seen)
        REQUIRE(value);
    REQUIRE(concurrent.Outstanding() == 0);
}
