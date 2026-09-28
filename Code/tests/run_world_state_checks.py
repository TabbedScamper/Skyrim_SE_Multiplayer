"""Compile isolated world-state checks with MSVC, without invoking xmake.

Uses the existing dependency manifests only for compiler flags and libraries.
All outputs live in a fresh temporary directory; the shared build is untouched.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
OUT = Path(tempfile.mkdtemp(prefix="world-state-checks-"))
DEPS = ROOT / "build/.deps"


def cached(target, suffix):
    data = (DEPS / target / "windows/x64/releasedbg" / suffix).read_text()
    values = data.split("values = {", 1)[1].split("\n        }", 1)[0]
    tokens = re.findall(r'\[\[(.*?)\]\]|"([^"\n]*)"', values)
    return [a or b for a, b in tokens if a or b]


def run(args, env):
    result = subprocess.run(args, cwd=ROOT, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(result.stdout, flush=True)
    if result.returncode:
        raise SystemExit(result.returncode)


compiler, *test_flags = cached("TPTests", "Code/tests/scene_timeline_encoding.cpp.obj.d")
vcvars = Path(compiler).parents[6] / "Auxiliary/Build/vcvars64.bat"
# This fixed command initializes MSVC only; no filesystem mutation uses cmd.
setup = subprocess.check_output(f'cmd.exe /d /s /c ""{vcvars}" >nul && set"', text=True)
env = os.environ.copy()
env.update(line.split("=", 1) for line in setup.splitlines() if "=" in line and not line.startswith("="))


def compile_file(source, target, manifest, syntax=False, forced=None):
    executable, *flags = cached(target, manifest)
    flags = [flag for flag in flags if not flag.lower().startswith(("-fd", "-fp", "-yu", "-fi", "-zi", "-o2"))]
    flags += ["/Od", "/Zs" if syntax else "/c"]
    if forced:
        flags += ["/FI" + str(ROOT / forced)]
    obj = OUT / (source.replace("/", "_") + ".obj")
    if not syntax:
        flags += ["/Fo" + str(obj)]
    run([executable, *flags, source], env)
    return obj


objects = []
sources = ["Code/encoding/Messages/WorldState.cpp", "Code/encoding/Messages/ClientMessageFactory.cpp", "Code/encoding/Messages/ServerMessageFactory.cpp"]
# Concurrent engineers can register new messages before the coordinator rebuilds
# the encoding library. Compile those additions read-only into this test binary.
additions = subprocess.check_output(["git", "ls-files", "--others", "--exclude-standard", "Code/encoding"], cwd=ROOT, text=True)
sources += [path for path in additions.splitlines() if path.endswith(".cpp") and path not in sources]
for source in sources:
    objects.append(compile_file(source, "SkyrimEncoding", "Code/encoding/Messages/ClientMessageFactory.cpp.obj.d", forced="Code/encoding/EncodingPch.h"))
objects.append(compile_file("Code/tests/world_state_encoding.cpp", "TPTests", "Code/tests/scene_timeline_encoding.cpp.obj.d"))
objects.append(compile_file("Code/tests/world_state_r3.cpp", "TPTests", "Code/tests/scene_timeline_encoding.cpp.obj.d"))
objects.append(ROOT / "build/.objs/TPTests/windows/x64/releasedbg/Code/tests/main.cpp.obj")
linker, *flags = cached("TPTests", "TPTests.exe.d")
flags = [flag for flag in flags if not flag.lower().startswith(("-pdb:", "-debug"))]
exe = OUT / "world_state_tests.exe"
run([linker, *map(str, objects), *flags, "/out:" + str(exe)], env)
run([str(exe), "[encoding.world_state]"], env)

for source in ("Code/client/Services/Generic/WorldStateService.cpp", "Code/client/Games/Skyrim/TESObjectREFR.cpp", "Code/client/World.cpp", "Code/client/Services/Generic/GameTestService.cpp"):
    compile_file(source, "SkyrimTogetherClient", "Code/client/Games/Skyrim/TESObjectREFR.cpp.obj.d", syntax=True, forced="Code/client/TiltedOnlinePCH.h")
for source in ("Code/server/Services/WorldStateService.cpp", "Code/server/World.cpp"):
    compile_file(source, "SkyrimTogetherServer", "Code/server/Services/SceneTimelineService.cpp.obj.d", syntax=True, forced="Code/server/Pch.h")
for source in ("Code/encoding/Messages/WorldState.cpp", "Code/tests/world_state_encoding.cpp", "Code/tests/world_state_r3.cpp"):
    if source.startswith("Code/encoding"):
        compile_file(source, "SkyrimEncoding", "Code/encoding/Messages/ClientMessageFactory.cpp.obj.d", syntax=True, forced="Code/encoding/EncodingPch.h")
    else:
        compile_file(source, "TPTests", "Code/tests/scene_timeline_encoding.cpp.obj.d", syntax=True)
print(f"World-state checks passed. Isolated artifacts: {OUT}")
