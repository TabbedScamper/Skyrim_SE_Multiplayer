"""Exercise production scheduling helpers with MSVC, without xmake or the game.

This measures identity scheduling, not Havok/scene access or in-game scan latency.
Native lifetime, locking and visual convergence still require paired game tests.
"""
from pathlib import Path
import os
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[2]
    header = (root / "Code/client/Services/ObjectService.h").read_text()
    helper = header.split("// BEGIN PHYSICS SCAN SET\n", 1)[1].split(
        "// END PHYSICS SCAN SET", 1
    )[0]
    source = r'''
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
''' + helper + r'''
int main() {
    // Callback admission works without load/attach notifications, including duplicates
    // and hash collisions. Unload/reload is resolved by the consumer's current state.
    PhysicsScan::MovementQueue<8> queue;
    for (uint32_t id = 1; id <= 8; ++id) assert(queue.Insert(id * 8));
    assert(queue.Insert(8));
    assert(!queue.Insert(99));
    size_t visited{};
    bool overflowSeen{};
    queue.Drain([&](uint32_t id) {
        assert(id == 99 || (id && id % 8 == 0));
        overflowSeen |= id == 99;
        ++visited;
    });
    assert(visited == 9 && overflowSeen && queue.Count == 0 && queue.Overflow.empty());
    assert(queue.Insert(99));
    queue.Drain([&](uint32_t id) { assert(id == 99); });

    // Same mutex handoff used by the native callback and frame consumer.
    PhysicsScan::MovementQueue<4096> concurrent;
    std::mutex lock;
    std::array<std::thread, 5> producers;
    for (uint32_t owner = 0; owner < 5; ++owner)
        producers[owner] = std::thread([&, owner] {
            for (uint32_t id = 1; id <= 500; ++id) {
                std::lock_guard guard(lock);
                assert(concurrent.Insert(owner * 500 + id));
            }
        });
    for (auto& thread : producers) thread.join();
    std::array<bool, 2501> seen{};
    concurrent.Drain([&](uint32_t id) { assert(!seen[id]); seen[id] = true; });
    for (uint32_t id = 1; id <= 2500; ++id) assert(seen[id]);

    // A network hole is a held target, never an unavailable window. Test boundaries,
    // long pauses, a single packet and recovery with a fresh packet.
    std::array<uint64_t, 3> ticks{1000, 1050, 1100};
    const auto at = [&](uint32_t i) { return ticks[i]; };
    assert(!PhysicsScan::SelectPlaybackWindow(0, 0, at).Available);
    auto middle = PhysicsScan::SelectPlaybackWindow(3, 1075, at);
    assert(middle.Available && middle.A == 1 && middle.B == 2 && middle.Fraction == .5f);
    for (double gap : {299., 300., 301., 500., 1000., 1500., 1501., 10000.}) {
        auto held = PhysicsScan::SelectPlaybackWindow(3, 1100 + gap, at);
        assert(held.Available && held.A == 2 && held.B == 2);
        assert(held.Hold == (gap > 300.));
    }
    auto single = PhysicsScan::SelectPlaybackWindow(1, 3000, at);
    assert(single.Available && single.Hold && single.A == 0 && single.B == 0);
    ticks[2] = 2000;
    assert(!PhysicsScan::SelectPlaybackWindow(3, 1990, at).Hold);

    // Saturation reserves a slot for every new tick; no skipped capture and no
    // stale LastSent deadline. Ordered timestamps identify any evicted history.
    size_t read{}, count{}, evictions{};
    std::array<uint64_t, 8> snapshots{};
    uint64_t lastSent{};
    for (uint64_t tick = 1; tick <= 100; ++tick) {
        evictions += PhysicsScan::MakeSnapshotRoom(read, count, snapshots.size());
        snapshots[(read + count) % snapshots.size()] = tick;
        ++count;
        lastSent = tick;
    }
    assert(lastSent == 100 && count == 8 && evictions == 92);
    for (size_t i = 0; i < 8; ++i) assert(snapshots[(read + i) % 8] == 93 + i);

    // Actual repair iterator has a fixed budget regardless of admitted population.
    // Native work is represented by a checksum only; these are scheduling timings.
    volatile uint64_t sink{};
    for (uint32_t admitted : {100u, 10000u, 100000u}) {
        PhysicsScan::Set sleeping, moving;
        for (uint32_t id = 1; id <= admitted; ++id) sleeping.Insert(id);
        for (uint32_t id = 1; id <= 16; ++id) { sleeping.Erase(id); moving.Insert(id); }
        size_t cursor{}, calls{};
        const auto start = std::chrono::steady_clock::now();
        for (size_t scan = 0; scan < 100000; ++scan) {
            for (uint32_t id : moving.Ids) { sink = sink + id; ++calls; }
            PhysicsScan::VisitRepair(sleeping, cursor, 16,
                [&](uint32_t id) { sink = sink + id; ++calls; });
        }
        assert(calls == 3200000);
        const auto us = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - start).count() / 100000;
        std::cout << admitted << " admitted, 16 moving + 16 repair: " << us
                  << " us/scheduling pass, " << calls << " visits\n";
    }
    std::cout << "PASS: five producers, movement admission/overflow/reuse, held gaps, "
                 "queue pressure, bounded repair and scheduling scale\n";
}
'''
    # Architectural regression checks supplement the executable helper tests.
    client = (root / "Code/client/Services/Generic/ObjectService.cpp").read_text()
    solver = client.split("int HookNativeStep(", 1)[1].split(
        "static TiltedPhoques::Initializer s_nativeStepHook", 1
    )[0]
    assert "spdlog::" not in solver and "CollectChildBodies" not in solver
    assert "SetMotionType(TESObjectREFR::MotionType::Keyframed" not in client
    assert "s_detachedSteeringConstraints" not in client
    update = client.split("void ObjectService::OnUpdate(", 1)[1].split(
        "void ObjectService::RefreshPhysicsDiagnostics", 1
    )[0]
    assert "CaptureHostPhysics(" not in update and "ApplyRemotePhysics(" not in update
    assert "RefreshPhysicsCandidates(" not in update
    with tempfile.TemporaryDirectory(prefix="physics-review-") as temp:
        directory = Path(temp)
        cpp = directory / "test.cpp"
        cpp.write_text(source)
        vcvars = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / (
            "Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat"
        )
        batch = directory / "check.cmd"
        batch.write_text(
            f'@call "{vcvars}" >nul\n'
            f'@cl /nologo /std:c++20 /EHsc /W4 /Zs "{cpp}"\n'
            '@if errorlevel 1 exit /b 1\n'
            f'@cl /nologo /std:c++20 /EHsc /O2 /W4 /Fe:test.exe "{cpp}"\n'
        )
        subprocess.run([str(batch)], cwd=directory, check=True)
        subprocess.run([str(directory / "test.exe")], cwd=directory, check=True)


if __name__ == "__main__":
    main()
