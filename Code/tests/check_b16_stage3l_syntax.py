"""Read-only MSVC syntax checks using the original build's dependency manifests."""

import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
ORIGINAL = Path(r"C:\Users\mwalt\SkyrimSeamlessCoop-ui")
UNITS = (
    ("SkyrimTogetherServer", "Code/server/Services/PartyService.cpp"),
    ("SkyrimTogetherServer", "Code/server/Services/DropInService.cpp"),
    ("SkyrimTogetherClient", "Code/client/Services/Generic/PartyService.cpp"),
    ("SkyrimTogetherClient", "Code/client/Services/Generic/GameTestService.cpp"),
    ("TPTests", "Code/tests/host_handoff_protocol.cpp"),
)


def command_for(target, source):
    manifest = ORIGINAL / "build/.deps" / target / "windows/x64/releasedbg" / (source + ".obj.d")
    data = manifest.read_text()
    values = data.split("values = {", 1)[1].split("\n        }", 1)[0]
    tokens = [a or b for a, b in re.findall(r"\[\[(.*?)\]\]|\"([^\"\n]*)\"", values) if a or b]
    compiler, *flags = tokens
    flags = [flag for flag in flags if not flag.lower().startswith(("-fd", "-fp", "-yu", "-zi", "-o2"))]
    flags += ["/Od", "/Zs", "/I" + str(ORIGINAL / "Libraries"),
              "/I" + str(ORIGINAL / "Libraries/TiltedConnect/Code/connect/include"),
              "/I" + str(ORIGINAL / "Libraries/TiltedReverse/Code/reverse/include"),
              "/I" + str(ORIGINAL / "Libraries/TiltedUI/Code/ui/include"),
              "/I" + str(ORIGINAL / "Libraries/TiltedHooks/Code/hooks/include"),
              "/I" + str(ORIGINAL / "build")]
    return compiler, flags


compiler, _ = command_for(*UNITS[0])
vcvars = Path(compiler).parents[6] / "Auxiliary/Build/vcvars64.bat"
setup = subprocess.check_output(f'cmd.exe /d /s /c ""{vcvars}" >nul && set"', text=True)
env = os.environ.copy()
env.update(line.split("=", 1) for line in setup.splitlines() if "=" in line and not line.startswith("="))

for target, source in UNITS:
    compiler, flags = command_for(target, source)
    result = subprocess.run([compiler, *flags, str(ROOT / source)], cwd=ROOT, env=env,
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(source, result.stdout, flush=True)
    if result.returncode:
        raise SystemExit(result.returncode)
print("b16-stage3l syntax checks passed")
