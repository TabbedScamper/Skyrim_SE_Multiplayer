"""Compile and exercise ObjectService's actual pure membership helper without xmake.

Run with Python 3 on Windows with MSVC installed, or with CXX on other systems.
The marked helper is extracted because the rest of ObjectService.h needs the game PCH.
All compiler products are kept in a temporary directory. This is not an in-game perf test.
"""

import os
from pathlib import Path
import shutil
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
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <set>
#include <unordered_map>
#include <vector>
static size_t allocations;
void* operator new(size_t size) {
    ++allocations;
    if (auto* ptr = std::malloc(size ? size : 1)) return ptr;
    throw std::bad_alloc();
}
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, size_t) noexcept { std::free(ptr); }
''' + helper + r'''
static void Check(const PhysicsScan::Set& actual, const std::set<uint32_t>& expected) {
    assert(actual.Ids.size() == expected.size());
    assert(actual.Indices.size() == expected.size());
    for (size_t i = 0; i < actual.Ids.size(); ++i) {
        assert(expected.contains(actual.Ids[i]));
        assert(actual.Indices.at(actual.Ids[i]) == i);
    }
}
int main() {
    PhysicsScan::Set candidates;
    candidates.Erase(1); // absent removal
    candidates.Insert(1);
    candidates.Insert(1); // repeated load/body-add event
    assert(candidates.Ids.size() == 1);
    candidates.Erase(1); // last element
    assert(candidates.Ids.empty() && candidates.Indices.empty());
    for (uint32_t id : {11, 22, 33}) candidates.Insert(id);
    candidates.Erase(22); // moved tail must get its new index
    Check(candidates, {11, 33});
    candidates.Erase(11); // first element
    candidates.Insert(22); // unload then reload
    Check(candidates, {22, 33});
    candidates.Clear(); // disconnect/authority reset
    Check(candidates, {});

    // Differential load/unload/reload/duplicate-event sequences for five independent owners.
    std::array<PhysicsScan::Set, 5> owners;
    std::array<std::set<uint32_t>, 5> expected;
    uint32_t rng = 0x3412;
    for (size_t step = 0; step < 50000; ++step) {
        rng = rng * 1664525u + 1013904223u;
        const auto owner = (rng >> 24) % owners.size();
        const auto id = (rng >> 8) % 512;
        switch (rng % 31) {
        case 0: owners[owner].Clear(); expected[owner].clear(); break;
        case 1: case 2: case 3: case 4: case 5: case 6: case 7:
            owners[owner].Erase(id); expected[owner].erase(id); break;
        default: owners[owner].Insert(id); expected[owner].insert(id); break;
        }
        Check(owners[owner], expected[owner]);
    }

    // Promotion between capture lanes preserves membership once and only once.
    PhysicsScan::Set passive, bodies;
    for (uint32_t id = 1; id <= 1000; ++id) passive.Insert(id);
    for (uint32_t id = 1; id <= 1000; id += 3) {
        passive.Erase(id); bodies.Insert(id); bodies.Insert(id);
    }
    assert(passive.Ids.size() + bodies.Ids.size() == 1000);
    for (const auto id : passive.Ids) assert(!bodies.Indices.contains(id));

    // Membership lookup/duplicate admission/steady capture iteration must not allocate.
    const auto before = allocations;
    uint64_t checksum{};
    for (size_t scan = 0; scan < 1000; ++scan) {
        for (const auto id : bodies.Ids) {
            bodies.Insert(id);
            checksum += id;
        }
    }
    assert(checksum != 0);
    assert(allocations == before);
    std::cout << "Physics scan set: lifecycle, 50000 randomized events, five owners, "
                 "lane promotion and allocation-free steady scans passed\n";
}
'''
    with tempfile.TemporaryDirectory(prefix="physics-scan-set-") as temp:
        directory = Path(temp)
        cpp = directory / "test.cpp"
        cpp.write_text(source)
        if os.name == "nt":
            vcvars = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / (
                "Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat"
            )
            executable = directory / "test.exe"
            command = (
                f'cl /nologo /std:c++20 /EHsc /W4 /Fe:"{executable}" "{cpp}"'
            )
            if not shutil.which("cl"):
                command = f'call "{vcvars}" >nul\n{command}'
            batch = directory / "check.cmd"
            batch.write_text("@echo off\n" + command + "\n")
            subprocess.run([str(batch)], cwd=directory, check=True)
        else:
            executable = directory / "test"
            subprocess.run(
                [os.environ.get("CXX", "c++"), "-std=c++20", str(cpp), "-o", str(executable)],
                cwd=directory, check=True,
            )
        subprocess.run([str(executable)], cwd=directory, check=True)


if __name__ == "__main__":
    main()
