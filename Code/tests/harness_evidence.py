"""Offline acceptance fault injection. Does not start or contact either game."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

source = Path(__file__).resolve().parents[2] / 'Tools/InGameTests/Analyze-Harness.py'
spec = importlib.util.spec_from_file_location('harness_analysis', source)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class EvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.stamp = '20260927-000000'
        self.status = dict(state='passed', run='42', epoch='9', stamp=self.stamp,
                           sequence=2, metric='collision-on', dropped=0, active=False, error='')
        self.accepted = {pc: copy.deepcopy(self.status) for pc in ('Host', 'Follower')}
        self.write_json('scenario.json', dict(metric='collision-on', steps=[dict(op='watch'), dict(op='finish')]))
        lines = ['Effective Data scan complete', 'Joined shared campaign']
        self.write_json('session-health.json', dict(serverBuildTag='test@dirty', serverPid=11, clients={
            pc: dict(logs=dict(pid=10, scan=True, joined=True, lines=lines)) for pc in self.accepted}))
        hashes = {name: 'a' * 64 for name in ('SkyrimTogether.exe', 'STServer.dll', 'SkyrimTogetherServer.exe', 'TPProcess.exe')}
        self.write_json('artifact-identity.json', dict(expected=hashes, clients={
            pc: dict(hashes=hashes, gamePid=10, serverPid=11) for pc in self.accepted}))
        for pc in self.accepted:
            folder = self.root / pc
            folder.mkdir()
            (folder / 'tp_client.log').write_text('\n'.join(lines), encoding='utf-8')
            self.write_rows(pc, [self.status], status=True)
            rows = []
            for sequence, operation in ((1, 'watch'), (2, 'finish')):
                for kind in ('step', 'precondition_ack', 'precondition_release', 'frame', 'step_done', 'barrier'):
                    rows.append(dict(kind=kind, run='42', epoch='9', sequence=sequence, op=operation, ready=True, frame=sequence))
            rows.append(dict(kind='actions_complete', run='42', epoch='9', sequence=2))
            self.write_rows(pc, rows)
        (self.root / 'Host/STServerOut.log').write_text('Harness server buildTag=test@dirty', encoding='utf-8')

    def write_json(self, name, value):
        (self.root / name).write_text(json.dumps(value), encoding='utf-8')

    def path(self, pc='Host', status=False):
        return self.root / pc / f'harness-{self.stamp}{".status" if status else ""}.jsonl'

    def write_rows(self, pc, rows, status=False):
        self.path(pc, status).write_text(''.join(json.dumps(row) + '\n' for row in rows), encoding='utf-8')

    def validate(self):
        return module.validate_capture(self.root, self.stamp, self.accepted)

    def test_complete_pair_passes(self):
        self.assertTrue(self.validate()['valid'])

    def party_fixture(self):
        wait = dict(x=100., y=200., z=300., radius=8, source='fixture')
        step = dict(op='walk', until_trigger='1093B4', party_trigger=True, party_wait=wait)
        self.write_json('scenario.json', dict(metric='collision-on', steps=[step, dict(op='finish')]))
        for pc in self.accepted:
            rows = list(module.records(self.path(pc)))
            rows[0].update(step)
            proof = dict(kind='local_trigger_enter', sequence=1, trigger=int('1093B4', 16), entrant=20, generation=1, leaderId=1, localId=1)
            if pc == 'Follower':
                proof = dict(wait, kind='party_trigger_proximity_arrived', sequence=1, trigger=int('1093B4', 16),
                             actualX=100., actualY=200., actualZ=303., triggerX=100., triggerY=200., triggerZ=400.,
                             targetDistance=3., triggerDistance=97., proximityTo='trigger', leaderId=1, localId=2)
            rows.insert(4, proof)
            self.write_rows(pc, rows)

    def test_party_local_and_proximity_pair(self):
        self.party_fixture()
        self.assertTrue(self.validate()['valid'])

    def test_party_leader_partner_trip(self):
        # 2026-09-30: the follower tripped 1093B4 first; the leader's step completes on the delivered trip.
        self.party_fixture()
        rows = list(module.records(self.path('Host')))
        rows[4].update(kind='party_trigger_enter', entrant=0xff000814)
        self.write_rows('Host', rows)
        self.assertTrue(self.validate()['valid'])
        for entrant in (0x14, 0):
            rows[4]['entrant'] = entrant
            self.write_rows('Host', rows)
            with self.assertRaises(ValueError):
                self.validate()

    def test_party_missing_proof(self):
        self.party_fixture()
        for pc in self.accepted:
            original = list(module.records(self.path(pc)))
            self.write_rows(pc, [r for r in original if r['kind'] not in ('local_trigger_enter', 'party_trigger_proximity_arrived')])
            with self.assertRaises(ValueError):
                self.validate()
            self.write_rows(pc, original)

    def test_party_leader_fallback(self):
        self.party_fixture()
        rows = list(module.records(self.path('Follower')))
        rows[4].update(proximityTo='leader', triggerZ=1000., triggerDistance=697.,
                       leaderId=1, leaderForm=0xff001234, leaderX=100., leaderY=200., leaderZ=350., leaderDistance=47.)
        self.write_rows('Follower', rows)
        self.assertTrue(self.validate()['valid'])
        for key, value in [('leaderId', 9), ('leaderForm', 0), ('leaderForm', None)]:
            original = rows[4][key]
            rows[4][key] = value
            self.write_rows('Follower', rows)
            with self.assertRaises(ValueError):
                self.validate()
            rows[4][key] = original
        rows[4].update(leaderZ=950., leaderDistance=647.)
        self.write_rows('Follower', rows)
        with self.assertRaises(ValueError):
            self.validate()

    def test_party_invalid_proof(self):
        self.party_fixture()
        faults = [('Host', 'kind', 'party_trigger_proximity_arrived'), ('Host', 'entrant', 21),
                  ('Follower', 'trigger', 1), ('Follower', 'sequence', 2),
                  ('Follower', 'actualX', 110.), ('Follower', 'actualX', float('nan')),
                  ('Follower', 'triggerX', 900.), ('Follower', 'targetDistance', 0.)]
        for pc, key, value in faults:
            with self.subTest(pc=pc, key=key, value=value):
                original = list(module.records(self.path(pc)))
                changed = copy.deepcopy(original)
                changed[4][key] = value
                self.write_rows(pc, changed)
                with self.assertRaises(ValueError):
                    self.validate()
                self.write_rows(pc, original)

    def test_missing_required_logs(self):
        for name in ('Host/tp_client.log', 'Follower/tp_client.log', 'Host/STServerOut.log'):
            with self.subTest(name=name):
                path = self.root / name
                original = path.read_bytes()
                path.unlink()
                with self.assertRaises((ValueError, OSError)):
                    self.validate()
                path.write_bytes(original)

    def test_truncation_and_malformed_records(self):
        for pc in self.accepted:
            for status in (False, True):
                path = self.path(pc, status)
                original = path.read_bytes()
                for suffix in (original[:-1], original[:-8], original + b'{broken}\n', b''):
                    with self.subTest(pc=pc, status=status, length=len(suffix)):
                        path.write_bytes(suffix)
                        with self.assertRaises((ValueError, KeyError)):
                            self.validate()
                path.write_bytes(original)

    def test_missing_complete_record(self):
        original = list(module.records(self.path()))
        for kind in ('actions_complete', 'precondition_ack', 'precondition_release', 'step_done', 'barrier'):
            with self.subTest(kind=kind):
                self.write_rows('Host', [row for row in original if row['kind'] != kind])
                with self.assertRaises(ValueError):
                    self.validate()

    def test_terminal_status_mismatch_or_drops(self):
        for key, value in (('run', '43'), ('epoch', '10'), ('state', 'running'), ('dropped', 1), ('active', True), ('sequence', 1)):
            with self.subTest(key=key):
                row = dict(self.status, **{key: value})
                self.write_rows('Host', [row], status=True)
                with self.assertRaises(ValueError):
                    self.validate()

    def test_cross_pc_run_mismatch(self):
        self.accepted['Follower']['run'] = '43'
        self.write_rows('Follower', [self.accepted['Follower']], status=True)
        rows = list(module.records(self.path('Follower')))
        for row in rows:
            row['run'] = '43'
        self.write_rows('Follower', rows)
        with self.assertRaises(ValueError):
            self.validate()

    def test_valid_json_after_terminal_is_rejected(self):
        with self.path().open('a') as stream:
            stream.write('{}\n')
        with self.assertRaises(ValueError):
            self.validate()

    def test_dirty_build_hash_or_process_mismatch(self):
        path = self.root / 'artifact-identity.json'
        original = json.loads(path.read_text())
        for mismatch in ('hash', 'gamePid', 'serverPid'):
            with self.subTest(mismatch=mismatch):
                data = copy.deepcopy(original)
                if mismatch == 'hash':
                    data['clients']['Host']['hashes']['STServer.dll'] = 'b' * 64
                else:
                    data['clients']['Host'][mismatch] += 1
                self.write_json('artifact-identity.json', data)
                with self.assertRaises(ValueError):
                    self.validate()

    def test_missing_frame_record(self):
        rows = list(module.records(self.path()))
        self.write_rows('Host', [row for row in rows if not (row['kind'] == 'frame' and row['frame'] == 1)])
        with self.assertRaises(ValueError):
            self.validate()

    def test_changed_scenario_parameter(self):
        self.write_json('scenario.json', dict(metric='collision-on', steps=[dict(op='watch'), dict(op='finish', cell='5DE24')]))
        with self.assertRaises(ValueError):
            self.validate()


if __name__ == '__main__':
    unittest.main()
