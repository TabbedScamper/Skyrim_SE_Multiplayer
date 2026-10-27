"""MSVC /Zs checks for the quest protocol batch; does not build or link."""
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
ORIGINAL = Path(r"C:\Users\mwalt\SkyrimSeamlessCoop-ui")
UNITS = {
    "SkyrimTogetherClient": [
        "Code/client/Games/Skyrim/Forms/TESQuest.cpp",
        "Code/client/Services/Generic/QuestService.cpp",
        "Code/client/SkyrimVM64.cpp",
    ],
    "SkyrimTogetherServer": ["Code/server/Services/QuestService.cpp"],
    "SkyrimEncoding": [
        "Code/encoding/Messages/QuestAliasFills.cpp",
        "Code/encoding/Messages/NotifyQuestAliasFills.cpp",
    ],
    "TPTests": ["Code/tests/encoding.cpp"],
}


def command(target, source):
    manifest = ORIGINAL / "build/.deps" / target / "windows/x64/releasedbg" / (source + ".obj.d")
    data = manifest.read_text()
    values = data.split("values = {", 1)[1].split("\n        }", 1)[0]
    tokens = [a or b for a, b in re.findall(r'\[\[(.*?)\]\]|"([^"\n]*)"', values) if a or b]
    compiler, *flags = tokens
    flags = [flag for flag in flags if not flag.lower().startswith(("-fd", "-fp", "-yu", "-fi", "-zi", "-o2"))]
    local = [ROOT / "Code", ROOT / "Code/encoding", ROOT / "Code/common"]
    if target == "SkyrimTogetherClient":
        local = [ROOT / "Code/client", ROOT / "Code/client/Games/Skyrim",
                 ROOT / "Code/client/Services", *local]
    elif target == "SkyrimTogetherServer":
        local = [ROOT / "Code/server", *local]
    flags = [*("/I" + str(path) for path in local), *flags, "/Od", "/Zs"]
    if target == "SkyrimTogetherClient":
        flags.append("/FI" + str(ROOT / "Code/client/TiltedOnlinePCH.h"))
    elif target == "SkyrimTogetherServer":
        flags.append("/FI" + str(ROOT / "Code/server/Pch.h"))
    elif target == "SkyrimEncoding":
        flags.append("/FI" + str(ROOT / "Code/encoding/EncodingPch.h"))
    vcvars = Path(compiler).parents[6] / "Auxiliary/Build/vcvars64.bat"
    return compiler, flags, vcvars


for target, sources in UNITS.items():
    for source in sources:
        compiler, flags, vcvars = command(target, source)
        setup = subprocess.check_output(f'cmd.exe /d /s /c ""{vcvars}" >nul && set"', text=True)
        env = os.environ.copy()
        env.update(line.split("=", 1) for line in setup.splitlines() if "=" in line and not line.startswith("="))
        result = subprocess.run([compiler, *flags, str(ROOT / source)], cwd=ORIGINAL, env=env,
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print(source, result.stdout, flush=True)
        if result.returncode:
            raise SystemExit(result.returncode)
print("b14b syntax checks passed")
