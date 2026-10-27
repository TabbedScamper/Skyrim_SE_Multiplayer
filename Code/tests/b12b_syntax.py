"""Run syntax checks for the b12b client cuts using the original build's flags.

No xmake invocation, object file, or PCH output is produced.
"""
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
ORIGINAL = Path(r"C:\Users\mwalt\SkyrimSeamlessCoop-ui")
MANIFEST = (ORIGINAL / "build/.deps/SkyrimTogetherClient/windows/x64/releasedbg/"
            "Code/client/Games/Skyrim/TESObjectREFR.cpp.obj.d").read_text()
values = MANIFEST.split("values = {", 1)[1].split("\n        }", 1)[0]
tokens = re.findall(r"\[\[(.*?)\]\]|\"([^\"\n]*)\"", values)
compiler, *flags = [a or b for a, b in tokens if a or b]
flags = [flag for flag in flags if not flag.lower().startswith(("-fd", "-fp", "-yu", "-fi", "-zi", "-o2"))]
fixed = []
for flag in flags:
    if flag.lower().startswith("-i") and not flag.lower().startswith("-imsvc"):
        prefix, value = flag[:2], flag[2:]
        path = Path(value)
        if not path.is_absolute():
            path = (ROOT / path) if (ROOT / path).exists() else (ORIGINAL / path)
        fixed.append(prefix + str(path))
    else:
        fixed.append(flag)
fixed += ["/Od", "/Zs", "/FI" + str(ROOT / "Code/client/TiltedOnlinePCH.h")]
vcvars = Path(compiler).parents[6] / "Auxiliary/Build/vcvars64.bat"
setup = subprocess.check_output(f'cmd.exe /d /s /c ""{vcvars}" >nul && set"', text=True)
env = os.environ.copy()
env.update(line.split("=", 1) for line in setup.splitlines() if "=" in line and not line.startswith("="))
for name in ("SkyrimVM64.cpp", "Services/Generic/CheckpointSaves.cpp",
             "Services/Generic/PartyService.cpp", "Games/Skyrim/BSGraphics/BSGraphicsRenderer.cpp",
             "Services/Generic/GameTestService.cpp", "Services/Generic/DoorVoteService.cpp",
             "Services/Generic/LobbyService.cpp"):
    source = ROOT / "Code/client" / name
    print(f"Checking {name}", flush=True)
    result = subprocess.run([compiler, *fixed, str(source)], cwd=ROOT, env=env, text=True)
    if result.returncode:
        raise SystemExit(result.returncode)
print("b12b syntax checks passed")
