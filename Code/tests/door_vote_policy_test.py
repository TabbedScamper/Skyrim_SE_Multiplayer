"""Compile-time unit tests of the production DoorVotePolicy, without Skyrim or xmake.

Run with Python 3 and MSVC cl, clang++, or g++ (optional CXX environment variable).
The namespace is extracted verbatim because the rest of DoorVoteService.h requires
the client's PCH, entt and transport types. No decision logic is copied into tests.
"""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class DoorVotePolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parents[2]
        header = (root / "Code/client/Services/DoorVoteService.h").read_text()
        begin = header.index("namespace DoorVotePolicy\n")
        end = header.index("} // namespace DoorVotePolicy", begin)
        cls.policy = header[begin:end] + "}\nusing namespace DoorVotePolicy;\n"
        cls.compiler = os.environ.get("CXX") or shutil.which("cl") or shutil.which("clang++") or shutil.which("g++")
        if not cls.compiler:
            vs = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "Microsoft Visual Studio"
            candidates = sorted(vs.glob("*/Community/VC/Tools/MSVC/*/bin/Hostx64/x64/cl.exe"))
            if candidates:
                cls.compiler = str(candidates[-1])
        if not cls.compiler:
            raise RuntimeError("A C++ compiler is required; set CXX to its executable")

    def compile_assertions(self, assertions):
        with tempfile.TemporaryDirectory(prefix="door-vote-policy-") as directory:
            source = Path(directory) / "test.cpp"
            # No SDK or STL includes: these are the Windows x64 size/pointer integer types.
            source.write_text("using size_t = decltype(sizeof(0));\nusing uintptr_t = unsigned long long;\nusing uint32_t = unsigned int;\n" +
                              self.policy + assertions)
            if Path(self.compiler).stem.lower() == "cl":
                options = ["/nologo", "/std:c++20", "/Zs", "/WX"]
            else:
                options = ["-std=c++20", "-fsyntax-only", "-Werror"]
            result = subprocess.run([self.compiler, *options, str(source)], cwd=directory,
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_only_verified_input_sites_accept_the_player(self):
        self.compile_assertions("""
constexpr uintptr_t pick = 0x140751580;
static_assert(IsInputCall(0x140751692, pick));
static_assert(!IsInputCall(0x14075168D, pick)); // CALL instruction, not its return
static_assert(!IsInputCall(0x140751691, pick));
static_assert(!IsInputCall(0x140751693, pick));
static_assert(!IsInputCall(0x140A4472F, pick)); // Papyrus Activate, ID 56139
static_assert(!IsInputCall(0x1402F12DC, pick)); // recursive ActivateRef
static_assert(!IsInputCall(0, pick));
static_assert(!IsInputCall(0x112, 0)); // unresolved relocation
static_assert(!IsInputCall(0x73, 0, 0));
static_assert(IsInputCall(0x140769073, pick, 0x140769000)); // default perk-menu choice
static_assert(!IsInputCall(0x140769072, pick, 0x140769000));
static_assert(!IsInputCall(0x140A4472F, pick, 0x140769000));
static_assert(Decide(true,true,true,false,true,true,true,2) == Skip::NotPlayer);
""")

    def test_automatic_doors_hold_before_the_fade_only_on_entry(self):
        self.compile_assertions("""
static_assert(IsAutomaticEntry(1, true));
static_assert(!IsAutomaticEntry(0, true)); // approach label
static_assert(!IsAutomaticEntry(0, false));
static_assert(!IsAutomaticEntry(1, false)); // leaving must remain native
static_assert(!IsAutomaticEntry(2, true));
// Helgen automatic doors activate later from player update 40447, not the E-key site.
static_assert(!IsInputCall(0x140745951, 0x140751580, 0x140769000));
static_assert(Decide(true,true,true,true,IsAutomaticEntry(1,true),true,true,2) == Skip::None);
""")

    def test_all_initial_skip_reasons_and_party_sizes(self):
        self.compile_assertions("""
static_assert(Decide(false,false,false,false,false,false,false,0) == Skip::MissingReference);
static_assert(Decide(true,false,false,true,true,true,true,2) == Skip::NotDoor);
static_assert(Decide(true,true,false,true,true,true,true,2) == Skip::NotLoadDoor);
static_assert(Decide(true,true,true,false,true,true,true,2) == Skip::NotPlayer);
static_assert(Decide(true,true,true,true,false,true,true,2) == Skip::NotPlayerInput);
static_assert(Decide(true,true,true,true,true,false,true,2) == Skip::Offline);
static_assert(Decide(true,true,true,true,true,true,false,2) == Skip::NoParty);
static_assert(Decide(true,true,true,true,true,true,true,0) == Skip::TooFewMembers);
static_assert(Decide(true,true,true,true,true,true,true,1) == Skip::TooFewMembers);
static_assert(Decide(true,true,true,true,true,true,true,2) == Skip::None);
static_assert(Decide(true,true,true,true,true,true,true,5) == Skip::None);
static_assert(Decide(true,true,true,true,true,true,true,32) == Skip::None);
""")

    def test_every_combination_of_activation_prerequisites(self):
        self.compile_assertions("""
constexpr bool prerequisites()
{
    for (unsigned flags = 0; flags < 128; ++flags)
        for (size_t members = 0; members <= 5; ++members)
        {
            auto reason = Decide(flags & 1, flags & 2, flags & 4, flags & 8,
                flags & 16, flags & 32, flags & 64, members);
            if ((reason == Skip::None) != (flags == 127 && members >= 2)) return false;
        }
    return true;
}
static_assert(prerequisites());
""")

    def test_follow_never_crosses_a_load_boundary_or_a_pending_vote(self):
        self.compile_assertions("""
static_assert(CanFollow(false, 1, 1, 0, 0)); // same interior
static_assert(CanFollow(false, 1, 2, 10, 10)); // neighboring exterior cells
static_assert(!CanFollow(false, 1, 2, 0, 0)); // different interiors
static_assert(!CanFollow(false, 1, 2, 10, 0)); // outside to keep
static_assert(!CanFollow(false, 1, 2, 0, 10)); // keep to outside
static_assert(!CanFollow(false, 1, 2, 10, 20)); // different worldspaces
static_assert(!CanFollow(false, 0, 0, 0, 0));
static_assert(!CanFollow(false, 0, 2, 10, 10));
static_assert(!CanFollow(true, 1, 1, 0, 0)); // waiting, countdown and loading
static_assert(!CanFollow(true, 1, 2, 10, 10));
static_assert(!CanFollow(false, 1, 2, 10, 0)); // cancellation cannot enable cross-cell gather
""")

    def test_server_observed_departure_cancels_from_a_still_ready_voter(self):
        self.compile_assertions("""
static_assert(LostReadyVoter(true, 2, 1, true, false));
static_assert(LostReadyVoter(true, 5, 4, true, false));
static_assert(!LostReadyVoter(false, 2, 1, true, false)); // replacement vote
static_assert(!LostReadyVoter(true, 2, 1, false, false)); // server already withdrew this player
static_assert(!LostReadyVoter(true, 2, 1, true, true)); // committed native load
static_assert(!LostReadyVoter(true, 1, 2, true, false));
static_assert(!LostReadyVoter(true, 2, 2, true, false));
""")


if __name__ == "__main__":
    unittest.main(verbosity=2)
