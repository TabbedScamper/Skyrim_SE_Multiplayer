"""Isolated MSVC syntax checks. Never build/deploy/launch Skyrim or invoke xmake."""
from pathlib import Path

helper = Path(__file__).with_name("run_world_state_checks.py")
scope = {"__file__": str(helper), "__name__": "ragdoll_syntax"}
exec(compile(helper.read_text().split("\nobjects = []", 1)[0], str(helper), "exec"), scope)
check = scope["compile_file"]
for source in (
    "Code/client/Games/Skyrim/Actor.cpp",
    "Code/client/Services/Generic/CorpseRagdollService.cpp",
    "Code/client/Services/Generic/ObjectService.cpp",
    "Code/client/Services/Generic/HarnessService.cpp",
    "Code/client/Games/Skyrim/Havok/PoseCopyAuthority.cpp",
):
    check(source, "SkyrimTogetherClient", "Code/client/Games/Skyrim/TESObjectREFR.cpp.obj.d",
          syntax=True, forced="Code/client/TiltedOnlinePCH.h")
for source in ("CorpseRagdollRequest.cpp", "NotifyCorpseRagdoll.cpp"):
    check("Code/encoding/Messages/" + source, "SkyrimEncoding",
          "Code/encoding/Messages/ClientMessageFactory.cpp.obj.d", syntax=True,
          forced="Code/encoding/EncodingPch.h")
print("Ragdoll MSVC /Zs checks passed. No game build or runtime validation.")
