"""Build isolated TPTests [replay] checks, never xmake or the shared build.

--prove runs regression checks and then requires observed acceptance to fail in
exactly the recorded tests. --mode observed returns that nonzero exit code.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
DEPS = ROOT / 'build/.deps'


def cached(target, suffix):
    text = (DEPS / target / 'windows/x64/releasedbg' / suffix).read_text()
    values = text.split('values = {', 1)[1].split('\n        }', 1)[0]
    return [a or b for a, b in re.findall(r'\[\[(.*?)\]\]|"([^"\n]*)"', values) if a or b]


def check_copies():
    for block in json.loads((ROOT / 'Tools/Replay/source_blocks.json').read_text()):
        source = (ROOT / block['path']).read_text()
        start = source.index(block['start'])
        value = source[start:source.index(block['end'], start)]
        if hashlib.sha256(value.encode()).hexdigest() != block['sha256']:
            raise RuntimeError('Owned source drift in ' + block['name'] + '; inspect and refresh counterpart deliberately')
    policy = (ROOT / 'Code/client/Services/ReplaySyncPolicy.h').read_text()
    obj = (ROOT / 'Code/client/Services/ObjectService.h').read_text()
    window = obj[obj.index('struct PlaybackWindow\n'):obj.index('inline bool MakeSnapshotRoom')]
    if window not in policy:
        raise RuntimeError('SelectPlaybackWindow changed in owned source; refresh offline copy after coordination')
    door = (ROOT / 'Code/client/Services/DoorVoteService.h').read_text()
    door = door[door.index('namespace DoorVotePolicy\n'):door.index('} // namespace DoorVotePolicy') + len('} // namespace DoorVotePolicy')]
    if door.replace('DoorVotePolicy', 'ReplayDoorVotePolicy') not in policy:
        raise RuntimeError('DoorVotePolicy changed; refresh offline copy')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=['regression', 'observed'], default='regression')
    parser.add_argument('--prove', action='store_true')
    parser.add_argument('--fixture', type=Path, default=ROOT / 'Code/tests/fixtures/replay/recorded_failures.rpl')
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    check_copies()
    output = args.out or Path(tempfile.mkdtemp(prefix='coop-replay-'))
    output.mkdir(parents=True, exist_ok=True)
    compiler, *flags = cached('TPTests', 'Code/tests/scene_timeline_encoding.cpp.obj.d')
    vcvars = Path(compiler).parents[6] / 'Auxiliary/Build/vcvars64.bat'
    setup = subprocess.check_output(f'cmd.exe /d /s /c ""{vcvars}" >nul && set"', text=True)
    env = os.environ.copy()
    env.update(line.split('=', 1) for line in setup.splitlines() if '=' in line and not line.startswith('='))
    env['TP_REPLAY_FIXTURE'] = str(args.fixture.resolve())
    run_extract_tests = subprocess.run([os.sys.executable, str(ROOT / 'Code/tests/replay_extractor_test.py')], cwd=ROOT)
    if run_extract_tests.returncode:
        raise SystemExit(run_extract_tests.returncode)
    inputs = ['Code/tests/replay.cpp', 'Code/tests/replay_recording.h',
              'Code/client/Services/ReplaySyncMath.h', 'Code/client/Services/ReplaySyncPolicy.h',
              'Code/encoding/Messages/NpcInventory.cpp', 'Code/encoding/Messages/NpcInventory.h']
    hashes = {path: hashlib.sha256((ROOT/path).read_bytes()).hexdigest() for path in inputs}

    def run(command, name, allow_failure=False, quiet=False):
        start = time.perf_counter()
        result = subprocess.run(command, cwd=ROOT, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (output / (name + '.log')).write_text(result.stdout, encoding='utf-8')
        if not quiet:
            print(result.stdout, end='', flush=True)
        print(f'{name}: exit={result.returncode} elapsed={time.perf_counter()-start:.3f}s', flush=True)
        if result.returncode and not allow_failure:
            raise SystemExit(result.returncode)
        return result

    objects = []
    for source, target, manifest in [
        ('Code/tests/replay.cpp', 'TPTests', 'Code/tests/scene_timeline_encoding.cpp.obj.d'),
        ('Code/encoding/Messages/NpcInventory.cpp', 'SkyrimEncoding', 'Code/encoding/Messages/ClientMessageFactory.cpp.obj.d')]:
        executable, *options = cached(target, manifest)
        options = [x for x in options if not x.lower().startswith(('-fd', '-fp', '-yu', '-fi', '-zi', '-o2'))]
        obj = output / (Path(source).stem + '.obj')
        options += ['/Od', '/c', '/Fo' + str(obj)]
        if target == 'SkyrimEncoding':
            options += ['/FI' + str(ROOT / 'Code/encoding/EncodingPch.h')]
        run([executable, *options, str(ROOT / source)], 'compile-' + obj.stem)
        objects.append(obj)
    objects.append(ROOT / 'build/.objs/TPTests/windows/x64/releasedbg/Code/tests/main.cpp.obj')
    linker, *flags = cached('TPTests', 'TPTests.exe.d')
    flags = [flag for flag in flags if not flag.lower().startswith(('-pdb:', '-debug'))]
    binary = output / 'TPReplayTests.exe'
    run([linker, *map(str, objects), *flags, '/out:' + str(binary)], 'link')
    env['TP_REPLAY_MODE'] = args.mode
    result = run([str(binary), '[replay]'], args.mode, allow_failure=True)
    report = dict(mode=args.mode, exit=result.returncode, binary=str(binary), sources=hashes,
                  fixture_sha256=hashlib.sha256(args.fixture.read_bytes()).hexdigest(),
                  game_launched=False, xmake_invoked=False)
    if args.prove:
        if result.returncode:
            raise SystemExit(result.returncode)
        env['TP_REPLAY_MODE'] = 'observed'
        observed = run([str(binary), '[replay]', '--reporter', 'junit'], 'observed', allow_failure=True, quiet=True)
        import xml.etree.ElementTree as ET
        # Replay diagnostics are written before Catch's XML; parse from the root.
        xml = observed.stdout[observed.stdout.index('<?xml'):]
        document = ET.fromstring(xml)
        failed = [node.attrib['name'] for node in document.iter('testcase') if node.find('failure') is not None]
        expected = {'Recorded cart output violates the 30-unit step target',
                    'Recorded Ralof empty set distinguishes missing supply from a complete worn plan',
                    'Recorded cart median and horse gap exceed paired observation targets',
                    'Recorded camera coverage does not invent player intent'}
        if observed.returncode == 0 or set(failed) != expected:
            raise RuntimeError(f'Observed run did not fail in exactly the recorded regressions: {failed}')
        report['observed_exit'] = observed.returncode
        report['observed_failed_tests'] = failed
        print('PROOF: recorded cart and Ralof outputs fail; complete worn plan differs from no-supply replay.')
        print('Observed failures (including missing camera evidence):', '; '.join(failed))
    changed = [path for path, digest in hashes.items() if hashlib.sha256((ROOT/path).read_bytes()).hexdigest() != digest]
    if changed:
        raise RuntimeError('Concurrent source change during test build: ' + ', '.join(changed))
    check_copies()
    (output / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('Isolated artifacts:', output)
    raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
