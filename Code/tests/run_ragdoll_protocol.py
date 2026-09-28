"""Compile/run only the isolated protocol unit executable, never the game."""
from pathlib import Path
helper = Path(__file__).with_name("run_world_state_checks.py")
scope = {"__file__": str(helper), "__name__": "ragdoll_protocol"}
exec(compile(helper.read_text().split("\nobjects = []", 1)[0], str(helper), "exec"), scope)
check = scope["compile_file"]
objects = []
for source in ("CorpseRagdollRequest.cpp", "NotifyCorpseRagdoll.cpp"):
    objects.append(check("Code/encoding/Messages/" + source, "SkyrimEncoding",
                         "Code/encoding/Messages/ClientMessageFactory.cpp.obj.d", forced="Code/encoding/EncodingPch.h"))
objects.append(check("Code/tests/corpse_ragdoll_protocol.cpp", "TPTests", "Code/tests/scene_timeline_encoding.cpp.obj.d"))
objects.append(scope["ROOT"] / "build/.objs/TPTests/windows/x64/releasedbg/Code/tests/main.cpp.obj")
linker, *flags = scope["cached"]("TPTests", "TPTests.exe.d")
flags = [flag for flag in flags if not flag.lower().startswith(("-pdb:", "-debug"))]
exe = scope["OUT"] / "ragdoll_protocol.exe"
scope["run"]([linker, *map(str, objects), *flags, "/out:" + str(exe)], scope["env"])
scope["run"]([str(exe), "[corpse_ragdoll]"], scope["env"])
print("Isolated protocol artifacts:", scope["OUT"])
