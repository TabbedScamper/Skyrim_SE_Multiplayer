"""Isolated scale-pass tests/compile checks; never invokes xmake or writes build/.

Uses installed dependency flags from the coordinator's existing manifests.
Run with Python 3; --syntax also checks the changed production translation units.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DEPS = ROOT / "build/.deps"

def cached(target, suffix):
    data = (DEPS / target / "windows/x64/releasedbg" / suffix).read_text()
    values = data.split("values = {", 1)[1].split("\n        }", 1)[0]
    return [a or b for a, b in re.findall(r'\[\[(.*?)\]\]|"([^"\n]*)"', values) if a or b]

def read(path):
    return (ROOT / path).read_text()

def block(path, begin, end):
    return read(path).split(begin, 1)[1].split(end, 1)[0]

def function(path, name):
    return extract_function(read(path), name)

def extract_function(source, name):
    start = source.index(name)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--syntax", action="store_true")
    args = parser.parse_args()
    compiler, *flags = cached("TPTests", "Code/tests/scene_timeline_encoding.cpp.obj.d")
    vcvars = Path(compiler).parents[6] / "Auxiliary/Build/vcvars64.bat"
    setup = subprocess.check_output(f'cmd.exe /d /s /c ""{vcvars}" >nul && set"', text=True, stderr=subprocess.STDOUT)
    env = os.environ.copy()
    env.update(line.split("=", 1) for line in setup.splitlines() if "=" in line and not line.startswith("="))
    def run(command):
        result = subprocess.run(command, cwd=ROOT, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print(result.stdout, flush=True)
        if result.returncode:
            raise SystemExit(result.returncode)
    def clean(flags):
        return [f for f in flags if not f.lower().startswith(("-fd", "-fp", "-yu", "-fi", "-zi", "-o2", "-dndebug"))]
    with tempfile.TemporaryDirectory(prefix="scale-checks-") as temp:
        out = Path(temp)
        packages = Path(os.environ["LOCALAPPDATA"]) / ".xmake/packages"
        entt = next(packages.glob("e/entt/*/*/include"))
        snappy = next(packages.glob("s/snappy/*/*/include"))
        source = read("Code/tests/scale_cost_cases.inc")
        source = source.replace("// INSERT ENTITY INDEX", block("Code/client/Utils.cpp", "// BEGIN ENTITY LOOKUP INDEX", "void ShowHudMessage"))
        source = source.replace("// INSERT PLAYER MANAGER HEADER", read("Code/server/Game/PlayerManager.h").replace("#pragma once", ""))
        source = source.replace("// INSERT PLAYER MANAGER SOURCE", "\n".join(line for line in read("Code/server/Game/PlayerManager.cpp").splitlines() if not line.startswith("#include")))
        source = source.replace("// INSERT SEND HELPERS", block("Code/server/GameServer.cpp", "// BEGIN SERVER SEND SCRATCH", "// END SERVER SEND SCRATCH"))
        source = source.replace("// INSERT PACKET SOURCE", read("Libraries/TiltedConnect/Code/connect/src/Packet.cpp").replace('#include "SteamInterface.hpp"', ""))
        source = source.replace("// INSERT REAL TRANSPORT SEND", function("Libraries/TiltedConnect/Code/connect/src/Server.cpp", "void Server::Send("))
        source = source.replace("// INSERT PRUNE", block("Code/client/Services/Generic/ObjectService.cpp", "// BEGIN FOLLOW PROBE PRUNE", "// END FOLLOW PROBE PRUNE"))
        object_path = "Code/client/Services/Generic/ObjectService.cpp"
        source = source.replace("// INSERT FOLLOW PROBE", "struct FollowProbe" + block(object_path, "struct FollowProbe", "std::unordered_map<void*, FollowProbe>"))
        source = source.replace("// INSERT FOLLOW REPORT", function(object_path, "if (s_followProbesDirty || now >= s_nextFollowReport)"))
        source = source.replace("// INSERT DISCOVERY", function("Code/client/Services/Generic/DiscoveryService.cpp", "void DiscoveryService::VisitForms()"))
        before = subprocess.check_output(["git", "show", "25fa030d:Code/client/Services/Generic/DiscoveryService.cpp"], cwd=ROOT, text=True)
        source = source.replace("// INSERT OLD DISCOVERY", extract_function(before, "void DiscoveryService::VisitForms()").replace("DiscoveryService::", "DiscoveryBefore::"))
        source = source.replace("// INSERT NPC UPDATE", function("Code/client/Services/Generic/NpcLootService.cpp", "void NpcLootService::OnUpdate("))
        source = source.replace("// INSERT NPC MAIN FRAME", function("Code/client/Services/Generic/NpcLootService.cpp", "void NpcLootService::OnMainFrame("))
        source = source.replace("// INSERT NPC TARGET WATCH", block("Code/client/Services/Generic/NpcLootService.cpp", "// BEGIN NPC TARGET WATCH", "// END NPC TARGET WATCH"))
        source = source.replace("// INSERT NPC TARGET CHANGED", function("Code/client/Services/Generic/NpcLootService.cpp", "void NpcLootService::OnTargetChanged("))
        source = source.replace("// INSERT AWARENESS", function("Code/client/Games/Skyrim/Combat/PlayerCombat.cpp", "void GetAwareness("))
        source = source.replace("// INSERT OBSERVER HEADER", "class ObserverSnapshot" + block("Code/client/Games/Skyrim/Combat/PlayerCombat.h", "class ObserverSnapshot", "bool HasSnapshot"))
        source = source.replace("// INSERT OBSERVER SNAPSHOT", block("Code/client/Games/Skyrim/Combat/PlayerCombat.cpp", "// BEGIN OBSERVER SNAPSHOT", "// END OBSERVER SNAPSHOT"))
        old_player_combat = subprocess.check_output(["git", "show", "25fa030d:Code/client/Games/Skyrim/Combat/PlayerCombat.cpp"], cwd=ROOT, text=True)
        source = source.replace("// INSERT OLD OBSERVERS", extract_function(old_player_combat, "std::vector<Actor*> Observers()").replace("Observers()", "OldObservers()"))
        # The revision withdraws only scale1's new native-hook lookup indexes.
        old_utils = subprocess.check_output(["git", "show", "25fa030d:Code/client/Utils.cpp"], cwd=ROOT, text=True)
        for name in ["std::optional<entt::entity> FindEntityByServerId(", "std::optional<ActorOwnershipToken> GetLocalOwnershipToken(", "std::optional<ActorOwnershipToken> GetRemoteOwnershipToken("]:
            assert function("Code/client/Utils.cpp", name) == extract_function(old_utils, name)
        combat_path = "Code/client/Services/Generic/CombatService.cpp"
        old_combat = subprocess.check_output(["git", "show", "25fa030d:" + combat_path], cwd=ROOT, text=True)
        new_combat = re.sub(r"// BEGIN SCALE COMBAT TIMING.*?// END SCALE COMBAT TIMING", "", function(combat_path, "void CombatService::RunTargetUpdates("), flags=re.S)
        new_combat = new_combat.replace("const PlayerCombat::ObserverSnapshot observerSnapshot;\n    const auto& observers = observerSnapshot.Get();", "const auto observers = PlayerCombat::Observers();")
        assert re.sub(r"\s+", "", new_combat) == re.sub(r"\s+", "", extract_function(old_combat, "void CombatService::RunTargetUpdates("))
        print("25fa030d comparisons passed: generic/native Utils helpers; combat body normalized for timing and snapshot storage. Discovery and observer baselines loaded for differential execution.", flush=True)
        cpp = out / "scale.cpp"
        cpp.write_text(source)
        obj, exe = out / "scale.obj", out / "scale.exe"
        run([compiler, *clean(flags), "/Od", "/I" + str(entt), "/I" + str(snappy), "/ILibraries/TiltedConnect/Code/connect/include", "/c", str(cpp), "/Fo" + str(obj)])
        linker, *link_flags = cached("TPTests", "TPTests.exe.d")
        link_flags = [f for f in link_flags if not f.lower().startswith(("-pdb:", "-debug")) and f != "SkyrimEncoding.lib"]
        run([linker, str(obj), *link_flags, str(snappy.parent / "lib/snappy.lib"), "/out:" + str(exe)])
        run([str(exe)])
        if args.syntax:
            client = ["World.cpp", "Utils.cpp", "Services/Generic/CharacterService.cpp", "Services/Generic/CombatService.cpp", "Services/Generic/StealthService.cpp", "Services/Generic/DiscoveryService.cpp", "Services/Generic/NpcLootService.cpp", "Services/Generic/ObjectService.cpp", "Games/Skyrim/Combat/PlayerCombat.cpp"]
            server = ["GameServer.cpp", "Game/PlayerManager.cpp"]
            for target, prefix, paths, manifest, pch in [
                ("SkyrimTogetherClient", "Code/client/", client, "Code/client/Services/Generic/CharacterService.cpp.obj.d", "Code/client/TiltedOnlinePCH.h"),
                ("SkyrimTogetherServer", "Code/server/", server, "Code/server/GameServer.cpp.obj.d", "Code/server/Pch.h")]:
                compiler, *unit_flags = cached(target, manifest)
                for path in paths:
                    filename = prefix + path
                    before = hashlib.sha256((ROOT / filename).read_bytes()).hexdigest()
                    run([compiler, *clean(unit_flags), "/Od", "/Zs", "/FI" + str(ROOT / pch), filename])
                    assert before == hashlib.sha256((ROOT / filename).read_bytes()).hexdigest(), "Concurrent edit during check: " + filename
                    print("MSVC /Zs PASS " + filename + " SHA256=" + before, flush=True)
        print("Scale checks passed; shared build outputs untouched.")

if __name__ == "__main__":
    main()
