"""Build and run the isolated QSP serializer cases in a temporary directory."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
ORIGINAL = Path(r"C:\Users\mwalt\SkyrimSeamlessCoop-ui")


def cached(target, suffix):
    path = ORIGINAL / "build/.deps" / target / "windows/x64/releasedbg" / suffix
    data = path.read_text()
    values = data.split("values = {", 1)[1].split("\n        }", 1)[0]
    return [a or b for a, b in re.findall(r'\[\[(.*?)\]\]|"([^"\n]*)"', values) if a or b]


compiler, *_ = cached("TPTests", "Code/tests/encoding.cpp.obj.d")
vcvars = Path(compiler).parents[6] / "Auxiliary/Build/vcvars64.bat"
setup = subprocess.check_output(f'cmd.exe /d /s /c ""{vcvars}" >nul && set"', text=True)
env = os.environ.copy()
env.update(line.split("=", 1) for line in setup.splitlines() if "=" in line and not line.startswith("="))


def run(args):
    result = subprocess.run(args, cwd=ORIGINAL, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(result.stdout, flush=True)
    if result.returncode:
        raise SystemExit(result.returncode)


with tempfile.TemporaryDirectory(prefix="qsp-protocol-") as temp:
    temp = Path(temp)
    objects = []
    units = [
        ("SkyrimEncoding", "Code/encoding/Messages/QuestAliasFills.cpp"),
        ("SkyrimEncoding", "Code/encoding/Messages/RequestQuestAliasFills.cpp"),
        ("SkyrimEncoding", "Code/encoding/Messages/NotifyQuestAliasFills.cpp"),
        ("TPTests", "Code/tests/quest_stage_protocol.cpp"),
    ]
    for target, source in units:
        executable, *flags = cached(target, source + ".obj.d") if target == "SkyrimEncoding" else cached(target, "Code/tests/encoding.cpp.obj.d")
        flags = [flag for flag in flags if not flag.lower().startswith(("-fd", "-fp", "-yu", "-fi", "-zi", "-o2"))]
        flags = ["/I" + str(ROOT / "Code"), "/I" + str(ROOT / "Code/encoding"),
                 *flags, "/Od", "/c"]
        if target == "SkyrimEncoding":
            flags.append("/FI" + str(ROOT / "Code/encoding/EncodingPch.h"))
        obj = temp / (Path(source).stem + ".obj")
        run([executable, *flags, "/Fo" + str(obj), str(ROOT / source)])
        objects.append(obj)
    objects.append(ORIGINAL / "build/.objs/TPTests/windows/x64/releasedbg/Code/tests/main.cpp.obj")
    linker, *flags = cached("TPTests", "TPTests.exe.d")
    flags = [flag for flag in flags if not flag.lower().startswith(("-pdb:", "-debug"))]
    exe = temp / "quest_stage_protocol.exe"
    run([linker, *map(str, objects), *flags, "/out:" + str(exe)])
    run([str(exe), "[quest_stage_protocol]"])
