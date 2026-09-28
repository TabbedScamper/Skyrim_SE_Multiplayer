"""MSVC /Zs only. No object/link step, xmake, shared build writes or deployment."""
from pathlib import Path

helper = Path(__file__).with_name("run_world_state_checks.py")
scope = {"__file__": str(helper), "__name__": "world_animation_syntax"}
exec(compile(helper.read_text().split("\nobjects = []", 1)[0], str(helper), "exec"), scope)
check = scope["compile_file"]
for source in ("Code/client/Games/Skyrim/WorldAnimation.cpp", "Code/client/Services/Generic/WorldStateService.cpp"):
    check(source, "SkyrimTogetherClient", "Code/client/Games/Skyrim/TESObjectREFR.cpp.obj.d", syntax=True, forced="Code/client/TiltedOnlinePCH.h")
check("Code/server/Services/WorldStateService.cpp", "SkyrimTogetherServer", "Code/server/Services/SceneTimelineService.cpp.obj.d", syntax=True, forced="Code/server/Pch.h")
for source in ("WorldState.cpp", "ClientMessageFactory.cpp", "ServerMessageFactory.cpp"):
    check("Code/encoding/Messages/" + source, "SkyrimEncoding", "Code/encoding/Messages/ClientMessageFactory.cpp.obj.d", syntax=True, forced="Code/encoding/EncodingPch.h")
for source in ("world_state_encoding.cpp", "world_state_r3.cpp", "world_animation_protocol.cpp"):
    check("Code/tests/" + source, "TPTests", "Code/tests/scene_timeline_encoding.cpp.obj.d", syntax=True)
print("MSVC syntax and compile-time grammar checks passed; runtime C++ protocol cases were not executed (/Zs only).")
