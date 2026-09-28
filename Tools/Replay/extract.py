"""Offline capture normalizer. No game, bridge, deployment or xmake calls.

Use 32 worker *processes* across captures/files. Original evidence is immutable.
Unknown fields stay in payload; absent measurements are never filled with zeros.
"""
import argparse
import bisect
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
from datetime import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct

VERSION = 1
WORN = re.compile(r"Worn evidence: (publish|receive|settled) ([0-9A-F]+) server (\d+) epoch (\d+) seq (\d+).*?(?:owner|worn) \[([^]]*)\](?: local \[([^]]*)\])?")
ITEM = re.compile(r"([0-9A-F]+):([0-9A-F]+)@([0-9A-F]+)")
STAMP = re.compile(r"^\[([^]]+)\]")
# HarnessService's watched vehicle references. Classification only, never a policy exception.
CARTS = {0xB9DF3, 0xBB970}
HORSES = {0xB9DF2, 0xBB971}


def items(value):
    return sorted([dict(mod=int(m, 16), base=int(b, 16), slots=int(s, 16))
                   for m, b, s in ITEM.findall(value or "")], key=lambda x: (x['mod'], x['base'], x['slots']))


def side_of(path):
    for part in reversed(path.parts):
        if part.lower() in ('host', 'follower'):
            return part.title()
    name = path.name.lower()
    if name.startswith(('host_', 'host-')):
        return 'Host'
    if name.startswith(('follower_', 'follower-')):
        return 'Follower'
    return 'Unknown'  # Never infer authority from an unlabelled tp_client.log.


def sha(path):
    with path.open('rb') as handle:
        return hashlib.file_digest(handle, 'sha256').hexdigest()


def normalize(task):
    root, output, relative = map(Path, task)
    path = root / relative
    capture = relative.parts[0]
    side = side_of(relative)
    dest = output / 'streams' / relative.with_suffix(relative.suffix + '.replay.jsonl')
    dest.parent.mkdir(parents=True, exist_ok=True)
    digest = sha(path)
    counts = Counter()
    previous = {}
    peaks = []
    worn = []
    errors = []
    run = None
    epoch = None
    last_tick = None
    session = 0
    with path.open(encoding='utf-8-sig', errors='replace') as src, dest.open('w', encoding='utf-8', newline='\n') as out:
        def emit(kind, payload, line, tick=None, wall=None, phase='observation'):
            record = dict(schema='coop-replay', version=VERSION, capture=capture, peer=side,
                          run=run, epoch=epoch, session=session, tick=tick, wall_ms=wall,
                          phase=phase, kind=kind, payload=payload,
                          source=dict(path=relative.as_posix(), line=line, sha256=digest))
            out.write(json.dumps(record, separators=(',', ':'), allow_nan=False) + '\n')
            counts[kind] += 1
            return record

        if path.suffix == '.jsonl' and '.status.' not in path.name:
            for number, line in enumerate(src, 1):
                try:
                    row = json.loads(line)
                except (ValueError, TypeError) as exc:
                    errors.append(dict(line=number, error=str(exc)))
                    continue
                next_run = row.get('run', run)
                if next_run != run:
                    previous.clear()
                run, epoch = next_run, row.get('epoch', epoch)
                kind = row.get('kind', 'unknown')
                tick = row.get('tick')
                last_tick = tick if tick is not None else last_tick
                wall = row.get('wallMs')
                category = {'frame': 'body_frame', 'pose': 'actor_pose', 'scene': 'scene',
                            'probe': 'world_node', 'reference': 'reference', 'armor': 'armor',
                            'precondition_ack': 'quest_state', 'precondition_release': 'quest_state',
                            'local_trigger_enter': 'trigger', 'driver': 'movement_driver'}.get(kind, 'harness_event')
                record = emit(category, row, number, tick, wall)
                if kind != 'frame':
                    continue
                for ref in row.get('refs', []):
                    identity = ref['id']
                    if not ref.get('loaded') or not ref.get('exists') or 'p' not in ref:
                        previous.pop(identity, None)
                        continue
                    old = previous.get(identity)
                    if old and old['cell'] == row.get('cell') and old['tick'] < tick and old['frame'] + 1 == row['frame']:
                        step = math.dist(old['p'], ref['p'])
                        if side == 'Follower' and identity in CARTS and step > 30:
                            peaks.append(dict(kind='cart_step', unit='CartStepMetric', capture=capture,
                                entity=identity, tick=tick, previous_tick=old['tick'],
                                dt_ms=wall-old['wall'], before=old['p'], after=ref['p'],
                                step=step, source=record['source'], previous_source=old['source'],
                                evidence='consecutive loaded reference observations; not body commands'))
                    previous[identity] = dict(p=ref['p'], tick=tick, wall=wall, cell=row.get('cell'),
                                              frame=row['frame'], source=record['source'])
        elif path.suffix == '.log':
            for number, line in enumerate(src, 1):
                if 'Steam lobby: listening' in line:
                    session += 1
                match = WORN.search(line)
                if match:
                    phase, form, server, owner_epoch, sequence, owner, local = match.groups()
                    timestamp = STAMP.match(line)
                    stamp = timestamp.group(1) if timestamp else None
                    payload = dict(entity=int(form, 16), server=int(server), ownership_epoch=int(owner_epoch),
                                   sequence=int(sequence), owner=items(owner), local=items(local) if local is not None else None,
                                   instance_data='not_captured', log_time=stamp, log_phase=phase)
                    rec = emit('worn', payload, number, phase={'publish': 'send', 'receive': 'receive', 'settled': 'applied'}[phase])
                    # Retain all candidates; select a fixture only after exact paired identity matching.
                    if int(form, 16) == 0x2BF9E and (phase == 'publish' or (phase == 'settled' and payload['owner'] and payload['local'] == [])):
                        worn.append(rec)
                elif any(token in line for token in ('Naked guard evidence', 'Cart steer ', 'Cart assembly ', 'Ragdoll', 'ragdoll',
                        'World animation', 'world animation', 'camera pitch', 'Camera pitch', 'Head track',
                        'Trigger gate', 'Door vote', 'animation variable')):
                    emit('client_diagnostic', dict(text=line.rstrip()), number)
        elif path.suffix == '.json':
            try:
                payload = json.load(src)
                emit('snapshot', payload, 1)
                game = payload.get('game', {}) if isinstance(payload, dict) else {}
                if 'camera' in game:
                    # Player rotation and camera-state rotation are different measurements.
                    # A death camera's zero-filled inactive-state fields are not pitch intent.
                    emit('camera', dict(camera_state=game['camera'], authority=game.get('cameraAuthority'),
                        player_rotation_rad=game.get('player', {}).get('rotation'), pitch_rad=None,
                        intent_pitch_rad=None, coverage='rendered pitch and player intent not captured'),
                        1, game.get('worldTick'), game.get('sampleTimeMs'))
                for key, category in [('quests', 'quest_state'), ('questEvents', 'quest_event'),
                                      ('playerAnimation', 'actor_animation'), ('playerNativeAnimation', 'actor_animation'),
                                      ('nativeCameraUpdateTrace', 'camera_trace')]:
                    if key in game:
                        emit(category, dict(snapshot_field=key, value=game[key]), 1,
                             game.get('worldTick'), game.get('sampleTimeMs'))
            except ValueError as exc:
                errors.append(dict(line=1, error=str(exc)))
    # Copy churn is detected, not silently accepted as a reproducible fixture.
    if sha(path) != digest:
        raise RuntimeError(f'Capture changed during extraction: {path}')
    return dict(path=relative.as_posix(), sha256=digest, records=dict(counts), errors=errors,
                peaks=sorted(peaks, key=lambda x: x['step'], reverse=True)[:32], worn=worn)


def paired_worn(results):
    records = [r for result in results for r in result['worn']]
    def identity(row):
        p = row['payload']
        return (row['capture'], p['entity'], p['server'], p['ownership_epoch'], p['sequence'],
                tuple((i['mod'], i['base'], i['slots']) for i in p['owner']))
    publications = {}
    for row in records:
        if row['peer'] == 'Host' and row['phase'] == 'send':
            publications.setdefault(identity(row), []).append(row)
    for failure in records:
        if failure['peer'] != 'Follower' or failure['phase'] != 'applied':
            continue
        p = failure['payload']
        for host in publications.get(identity(failure), []):
            h = host['payload']
            delta = (datetime.fromisoformat(p['log_time']) - datetime.fromisoformat(h['log_time'])).total_seconds()
            # PC wall clocks are approximate; message identity, not this timestamp, is the pairing key.
            if abs(delta) > 10:
                continue
            return dict(kind='worn', unit='PlanNpcWorn', entity=p['entity'], owner=p['owner'], local=p['local'],
                        capture=failure['capture'], host_source=host['source'], source=failure['source'],
                        server=p['server'], epoch=p['ownership_epoch'], sequence=p['sequence'],
                        log_time=p['log_time'], evidence='paired published intent and settled empty follower set; item/slot projection')
    raise RuntimeError('No paired recorded Ralof settled-empty failure found')


def write_binary(cases, path):
    """RPL1: little endian count, then type, length-prefixed provenance and type payload.

    Case types: 1 cart step (6 f64), 2 worn (two arrays of u32 mod/base,u64 slots).
    JSONL is authoritative; binary is an intentionally small C++ runner transport.
    """
    with path.open('wb') as out:
        out.write(b'RPL1' + struct.pack('<I', len(cases)))
        for case in cases:
            text = json.dumps({k: v for k, v in case.items() if k not in ('before', 'after', 'owner', 'local', 'observed')}, separators=(',', ':'), sort_keys=True).encode()
            tag = {'cart_step': 1, 'worn': 2, 'cart_gap': 3, 'horse_gap': 4,
                   'camera': 5, 'bone': 6, 'debris': 7}[case['kind']]
            out.write(struct.pack('<II', tag, len(text)) + text)
            if tag == 1:
                out.write(struct.pack('<6d', *case['before'], *case['after']))
            elif tag == 2:
                for name in ('owner', 'local'):
                    out.write(struct.pack('<I', len(case[name])))
                    for item in case[name]:
                        out.write(struct.pack('<IIQ', item['mod'], item['base'], item['slots']))
            elif tag in (3, 4):
                out.write(struct.pack('<9d3Q', *case['before'], *case['after'], *case['observed'],
                                      case['first_tick'], case['second_tick'], case['tick']))
            elif tag == 5:
                values = [case.get(k) for k in ('pitch_rad', 'intent_pitch_rad', 'tolerance_rad')]
                mask = sum(1 << i for i, value in enumerate(values) if value is not None)
                out.write(struct.pack('<B3d', mask, *(value if value is not None else 0 for value in values)))
            elif tag == 6:
                out.write(struct.pack('<B14d', case['settled'], *case['target_position_havok'],
                    *case['observed_position_havok'], *case['target_rotation_xyzw'], *case['observed_rotation_xyzw']))
            elif tag == 7:
                out.write(struct.pack('<B6d', case['settled'], *case['before'], *case['after']))


def camera_coverage(root):
    relative = Path('spectate-20260927/spectate_snapshot.json')
    path = root / relative
    if not path.exists():
        return []
    snapshot = json.loads(path.read_text(encoding='utf-8-sig'))['game']
    return [dict(kind='camera', unit='CameraPitch', pitch_rad=None, intent_pitch_rad=None, tolerance_rad=None,
                 player_rotation_rad=snapshot['player']['rotation'], camera_state=snapshot['cameraAuthority']['localStateId'],
                 source=dict(path=relative.as_posix(), line=1, sha256=sha(path)),
                 evidence='spectate snapshot has player pitch, but no validated rendered pitch or player intent')]


def motion_window(root, cart):
    """Pair a one-second regression window on the captured network clock.

    This is an observation comparison, not proof of packet presentation timing.
    Never join process-local wallMs across PCs or interpolate through a load gap.
    """
    frames = {}
    sources = {}
    for side in ('Host', 'Follower'):
        relative = Path(cart['capture']) / side / (cart['capture'] + '.jsonl')
        path = root / relative
        digest = sha(path)
        rows = []
        with path.open(encoding='utf-8-sig') as stream:
            for number, line in enumerate(stream, 1):
                row = json.loads(line)
                if row.get('kind') == 'frame' and abs(row['tick'] - cart['tick']) <= 750:
                    rows.append((row, dict(path=relative.as_posix(), line=number, sha256=digest)))
        frames[side] = rows
        sources[side] = digest
    host = sorted(frames['Host'], key=lambda x: x[0]['tick'])
    ticks = [r[0]['tick'] for r in host]
    result = []
    for follower, source in frames['Follower']:
        tick = follower['tick']
        if abs(tick - cart['tick']) > 500:
            continue
        i = bisect.bisect_right(ticks, tick)
        if not 0 < i < len(host):
            continue
        (first, first_source), (second, second_source) = host[i-1:i+1]
        if (second['tick'] - first['tick'] > 50 or first['cell'] != second['cell'] or
                first.get('run') != follower.get('run') or first['frame'] + 1 != second['frame']):
            continue
        first_refs = {r['id']: r for r in first.get('refs', []) if r.get('loaded') and r.get('exists')}
        second_refs = {r['id']: r for r in second.get('refs', []) if r.get('loaded') and r.get('exists')}
        for ref in follower.get('refs', []):
            identity = ref['id']
            if not ref.get('loaded') or identity not in first_refs or identity not in second_refs:
                continue
            if identity not in CARTS | HORSES:
                continue
            result.append(dict(kind='cart_gap' if identity in CARTS else 'horse_gap', unit='ActorPosition/GapMetric',
                entity=identity, capture=cart['capture'], first_tick=first['tick'], second_tick=second['tick'], tick=tick,
                before=first_refs[identity]['p'], after=second_refs[identity]['p'], observed=ref['p'],
                source=source, host_source=first_source, host_second_source=second_source,
                evidence='one-second reference observation window; aligned network clock, presentation delay unknown'))
    if not any(c['kind'] == 'cart_gap' for c in result) or not any(c['kind'] == 'horse_gap' for c in result):
        raise RuntimeError('No paired cart/horse observations around recorded cart failure')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--captures', type=Path, default=Path('C:/Tools/skyrim_re/agent/captures'))
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--fixtures', type=Path)
    parser.add_argument('--workers', type=int, default=32)
    args = parser.parse_args()
    if args.workers < 1:
        parser.error('--workers must be positive')
    args.out.mkdir(parents=True, exist_ok=True)
    paths = sorted(p.relative_to(args.captures) for p in args.captures.rglob('*')
                   if p.is_file() and p.suffix in ('.log', '.json', '.jsonl') and len(p.relative_to(args.captures).parts) > 1)
    with ProcessPoolExecutor(max_workers=args.workers) as pool:
        results = list(pool.map(normalize, [(str(args.captures), str(args.out), str(p)) for p in paths]))
    peaks = [p for result in results for p in result.pop('peaks')]
    # Explicit historical regression selection: the tracker's approximately 153-unit cart jump.
    recorded = [p for p in peaks if 151 <= p['step'] <= 155]
    if not recorded:
        raise RuntimeError('No 153-unit cart step found; largest=' + str(sorted(peaks, key=lambda x: -x['step'])[:3]))
    cart = min(recorded, key=lambda x: abs(x['step'] - 153))
    cases = [cart, paired_worn(results), *motion_window(args.captures, cart), *camera_coverage(args.captures)]
    for result in results:
        result.pop('worn')
    manifest = dict(version=VERSION, workers=args.workers, files=len(paths), sources=results,
                    coverage='Sparse observations. Packet ticks, velocities, bone rotations, camera intent may be absent.',
                    largest_cart_steps=sorted(peaks, key=lambda x: -x['step'])[:10])
    (args.out / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    fixture_dir = args.fixtures or args.out
    fixture_dir.mkdir(parents=True, exist_ok=True)
    (fixture_dir / 'recorded_failures.jsonl').write_text(''.join(json.dumps(c, sort_keys=True) + '\n' for c in cases), encoding='utf-8')
    write_binary(cases, fixture_dir / 'recorded_failures.rpl')
    print(json.dumps(dict(files=len(paths), workers=args.workers, records=sum(sum(r['records'].values()) for r in results),
                          malformed=sum(len(r['errors']) for r in results), cases=cases[:2],
                          gap_samples=sum(c['kind'] in ('cart_gap', 'horse_gap') for c in cases)), indent=2))


if __name__ == '__main__':
    main()
