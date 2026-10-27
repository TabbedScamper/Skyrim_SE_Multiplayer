"""Syntax check changed client units against the original read-only build manifest."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
ORIGINAL = Path(r"C:\Users\mwalt\SkyrimSeamlessCoop-ui")
MANIFEST = ORIGINAL / "build/.deps/SkyrimTogetherClient/windows/x64/releasedbg/Code/client/SkyrimVM64.cpp.obj.d"
data = MANIFEST.read_text()
values = data.split("values = {", 1)[1].split("\n        }", 1)[0]
tokens = [a or b for a, b in re.findall(r'\[\[(.*?)\]\]|"([^"\n]*)"', values) if a or b]
compiler, *flags = tokens
flags = [flag for flag in flags if not flag.lower().startswith(("-fd", "-fp", "-yu", "-fi", "-zi", "-o2"))]
flags += ["/Od", "/Zs", "/I" + str(ROOT / "Code/client"),
          "/FI" + str(ROOT / "Code/client/TiltedOnlinePCH.h")]
vcvars = Path(compiler).parents[6] / "Auxiliary/Build/vcvars64.bat"
setup = subprocess.check_output(f'cmd.exe /d /s /c ""{vcvars}" >nul && set"', text=True)
env = os.environ.copy()
env.update(line.split("=", 1) for line in setup.splitlines() if "=" in line and not line.startswith("="))
for source in ("Code/client/SkyrimVM64.cpp", "Code/client/Services/Generic/GameTestService.cpp",
               "Code/client/Services/Generic/LobbyService.cpp", "Code/client/Services/Generic/CalendarService.cpp",
               "Code/client/Services/Generic/TransportService.cpp",
               "Code/client/Games/Skyrim/BSGraphics/BSGraphicsRenderer.cpp"):
    result = subprocess.run([compiler, *flags, str(ROOT / source)], cwd=ORIGINAL, env=env,
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(result.stdout, flush=True)
    if result.returncode:
        raise SystemExit(result.returncode)
print("b11b syntax checks passed")
with tempfile.TemporaryDirectory(prefix="b11b-queue-") as out:
    executable = Path(out) / "main_queue_test.exe"
    test = subprocess.run([compiler, "/nologo", "/std:c++20", "/EHsc", "/O2",
                           str(ROOT / "Code/tests/main_queue_test.cpp"), "/Fe:" + str(executable),
                           "/Fo:" + str(Path(out) / "main_queue_test.obj")],
                          cwd=ROOT, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(test.stdout, flush=True)
    if test.returncode:
        raise SystemExit(test.returncode)
    subprocess.run([str(executable)], check=True, cwd=ROOT, env=env)
print("b11b MainQueue tests passed")
