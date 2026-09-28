"""Isolated MSVC syntax and protocol checks; never invokes xmake."""
import os
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
OUT = Path(tempfile.mkdtemp(prefix="hostorphan-checks-"))
print(f"Artifacts: {OUT}", flush=True)

# Reject a successful-looking run if another writer changed this task's input
# while MSVC was checking it. Other workstreams can continue editing their files.
INPUTS = (
    "Code/server/GameServer.cpp", "Code/server/Game/Player.cpp", "Code/server/Game/Player.h",
    "Code/server/Services/CharacterService.cpp", "Code/server/Services/CharacterService.h",
    "Code/server/Services/PartyService.cpp", "Code/server/Services/PlayerService.cpp",
    "Code/server/Components/OwnerComponent.h", "Code/server/Services/OwnershipPolicy.h",
    "Code/client/Services/Generic/OrphanTrace.cpp", "Code/client/Services/Generic/OrphanTrace.h",
    "Code/tests/server_ownership.cpp", "Code/tests/hostorphan_protocol.cpp",
)
before = {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in INPUTS}


def cached(target, suffix):
    data = (ROOT / "build/.deps" / target / "windows/x64/releasedbg" / suffix).read_text()
    values = data.split("values = {", 1)[1].split("\n        }", 1)[0]
    return [a or b for a, b in re.findall(r'\[\[(.*?)\]\]|"([^"\n]*)"', values) if a or b]


compiler, *_ = cached("TPTests", "Code/tests/scene_timeline_encoding.cpp.obj.d")
vcvars = Path(compiler).parents[6] / "Auxiliary/Build/vcvars64.bat"
setup_env = os.environ.copy()
installer = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer"
setup_env["PATH"] = str(installer) + os.pathsep + setup_env.get("PATH", "")
setup = subprocess.check_output(f'cmd.exe /d /s /c ""{vcvars}" >nul && set"', text=True, env=setup_env)
env = os.environ.copy()
env.update(line.split("=", 1) for line in setup.splitlines() if "=" in line and not line.startswith("="))


def run(args):
    result = subprocess.run(args, cwd=ROOT, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(result.stdout, flush=True)
    if result.returncode:
        raise SystemExit(result.returncode)


def compile_file(source, target, manifest, syntax=True, forced=None):
    executable, *flags = cached(target, manifest)
    flags = [f for f in flags if not f.lower().startswith(("-fd", "-fp", "-yu", "-fi", "-zi", "-o2"))]
    flags += ["/Od", "/Zs" if syntax else "/c"]
    if forced:
        flags += ["/FI" + str(ROOT / forced)]
    obj = OUT / (source.replace("/", "_") + ".obj")
    if not syntax:
        flags += ["/Fo" + str(obj)]
    run([executable, *flags, source])
    return obj


for source in ("Code/server/GameServer.cpp", "Code/server/Game/Player.cpp", "Code/server/Services/CharacterService.cpp",
               "Code/server/Services/PartyService.cpp", "Code/server/Services/PlayerService.cpp"):
    compile_file(source, "SkyrimTogetherServer", "Code/server/Services/CharacterService.cpp.obj.d", forced="Code/server/Pch.h")
for source in ("Code/client/Services/Generic/TransportService.cpp", "Code/client/Services/Generic/OrphanTrace.cpp", "Code/client/World.cpp"):
    compile_file(source, "SkyrimTogetherClient", "Code/client/Services/Generic/TransportService.cpp.obj.d", forced="Code/client/TiltedOnlinePCH.h")
compile_file("Code/encoding/Messages/AssignCharacterRequest.cpp", "SkyrimEncoding", "Code/encoding/Messages/ClientMessageFactory.cpp.obj.d", forced="Code/encoding/EncodingPch.h")
for source in ("Code/tests/server_ownership.cpp", "Code/tests/hostorphan_protocol.cpp"):
    compile_file(source, "TPTests", "Code/tests/scene_timeline_encoding.cpp.obj.d")

objects = []
sources = ["Code/encoding/Messages/AssignCharacterRequest.cpp", "Code/encoding/Messages/ClientMessageFactory.cpp"]
# Fresh factory registration may reference concurrent additions absent from the
# existing library. Compile those read-only into this isolated test executable.
additions = subprocess.check_output(["git", "ls-files", "--others", "--exclude-standard", "Code/encoding"], cwd=ROOT, text=True)
sources += [p for p in additions.splitlines() if p.endswith(".cpp") and p not in sources]
for source in sources:
    objects.append(compile_file(source, "SkyrimEncoding", "Code/encoding/Messages/ClientMessageFactory.cpp.obj.d", syntax=False, forced="Code/encoding/EncodingPch.h"))
objects.append(compile_file("Code/tests/hostorphan_protocol.cpp", "TPTests", "Code/tests/scene_timeline_encoding.cpp.obj.d", syntax=False))
objects.append(ROOT / "build/.objs/TPTests/windows/x64/releasedbg/Code/tests/main.cpp.obj")
linker, *flags = cached("TPTests", "TPTests.exe.d")
flags = [f for f in flags if not f.lower().startswith(("-pdb:", "-debug"))]
exe = OUT / "hostorphan_tests.exe"
run([linker, *map(str, objects), *flags, "/out:" + str(exe)])
run([str(exe), "[hostorphan]"])
after = {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in INPUTS}
(OUT / "checked-inputs.json").write_text(json.dumps(after, indent=2))
changed = [p for p in INPUTS if before[p] != after[p]]
if changed:
    raise SystemExit("Inputs changed during validation; rerun: " + ", ".join(changed))
print("Hostorphan checks passed.", flush=True)
