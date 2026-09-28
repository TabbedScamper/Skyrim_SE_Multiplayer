"""Headless extractor integrity checks using real capture evidence."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('replay_extract', ROOT / 'Tools/Replay/extract.py')
extract = importlib.util.module_from_spec(spec)
spec.loader.exec_module(extract)
FIXTURES = ROOT / 'Code/tests/fixtures/replay'


class ReplayExtractionTests(unittest.TestCase):
    def test_committed_binary_is_exactly_derived_from_jsonl(self):
        cases = [json.loads(line) for line in (FIXTURES / 'recorded_failures.jsonl').read_text().splitlines()]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'regenerated.rpl'
            extract.write_binary(cases, path)
            self.assertEqual(path.read_bytes(), (FIXTURES / 'recorded_failures.rpl').read_bytes())

    def test_fixture_provenance_points_to_real_unchanged_source(self):
        root = Path('C:/Tools/skyrim_re/agent/captures')
        if not root.exists():
            self.skipTest('external captures unavailable; committed binary integrity still tested')
        seen = {}
        for line in (FIXTURES / 'recorded_failures.jsonl').read_text().splitlines():
            case = json.loads(line)
            for key in ('source', 'previous_source', 'host_source', 'host_second_source'):
                if key not in case:
                    continue
                source = case[key]
                path = root / source['path']
                if path not in seen:
                    data = path.read_bytes()
                    seen[path] = (hashlib.sha256(data).hexdigest(), data.decode('utf-8-sig').splitlines())
                digest, rows = seen[path]
                self.assertEqual(source['sha256'], digest)
                self.assertTrue(rows[source['line']-1])
            def source_line(key):
                ref = case[key]
                return seen[root / ref['path']][1][ref['line']-1]
            def position(key):
                row = json.loads(source_line(key))
                return next(ref['p'] for ref in row['refs'] if ref['id'] == case['entity'])
            if case['kind'] == 'cart_step':
                self.assertEqual(case['before'], position('previous_source'))
                self.assertEqual(case['after'], position('source'))
            elif case['kind'] in ('cart_gap', 'horse_gap'):
                self.assertEqual(case['before'], position('host_source'))
                self.assertEqual(case['after'], position('host_second_source'))
                self.assertEqual(case['observed'], position('source'))
            elif case['kind'] == 'worn':
                host = extract.WORN.search(source_line('host_source'))
                follower = extract.WORN.search(source_line('source'))
                self.assertEqual(host.group(1), 'publish')
                self.assertEqual(follower.group(1), 'settled')
                self.assertEqual(case['owner'], extract.items(host.group(6)))
                self.assertEqual(case['owner'], extract.items(follower.group(6)))
                self.assertEqual(case['local'], extract.items(follower.group(7)))
            elif case['kind'] == 'camera':
                snapshot = json.loads((root/case['source']['path']).read_text(encoding='utf-8-sig'))
                self.assertEqual(case['player_rotation_rad'], snapshot['game']['player']['rotation'])
        self.assertGreaterEqual(len(seen), 4)

    def test_unloaded_frames_do_not_create_fake_cart_steps(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / 'capture/Follower/harness.jsonl'
            path.parent.mkdir(parents=True)
            rows = [dict(kind='frame', run='1', tick=i*16, wallMs=i*16, frame=i, cell=1,
                         refs=[dict(id=0xBB970, exists=True, loaded=loaded, p=[position,0,0])])
                    for i, (loaded, position) in enumerate([(True,0),(False,500),(True,1000)],1)]
            path.write_text('\n'.join(json.dumps(row) for row in rows))
            result = extract.normalize((str(root), str(root/'out'), str(path.relative_to(root))))
            self.assertEqual(result['peaks'], [])
            self.assertEqual(result['errors'], [])

    def test_receive_and_settled_are_not_conflated_and_unknown_peer_stays_unknown(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / 'capture/tp_client.log'
            path.parent.mkdir()
            path.write_text('[2026-09-27 10:47:30.886] Worn evidence: receive 2BF9E server 5 epoch 1 seq 14 owner [ 0:A6D7F@80] local []\n')
            extract.normalize((str(root), str(root/'out'), str(path.relative_to(root))))
            row = json.loads((root/'out/streams/capture/tp_client.log.replay.jsonl').read_text())
            self.assertEqual(row['phase'], 'receive')
            self.assertEqual(row['peer'], 'Unknown')
            self.assertIsNone(row['tick'])
            self.assertEqual(row['payload']['local'], [])


if __name__ == '__main__':
    unittest.main()
