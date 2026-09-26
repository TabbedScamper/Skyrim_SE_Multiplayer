"""Exercise the ledger's actual SQL schema without compiling or building the mod.

This checks schema constraints and SQLite rollback, not the C++ Ledger methods.
Run CampaignCoreTests and TPTests when the coordinator builds the changed code.
"""
import pathlib
import re
import sqlite3
import unittest


SOURCE = (pathlib.Path(__file__).parents[1] / "CampaignLedger.cpp").read_text()
SCHEMA = [
    "".join(re.findall(r'"([^"\n]*)"', statement))
    for statement in re.findall(r'Execute\(("CREATE TABLE.*?);', SOURCE, re.S)
]


class QuestItemSchemaTests(unittest.TestCase):
    def setUp(self):
        self.db = sqlite3.connect(":memory:")
        self.addCleanup(self.db.close)
        self.db.execute("PRAGMA foreign_keys=ON")
        for sql in SCHEMA:
            self.db.execute(sql)
        self.db.execute("INSERT INTO journal VALUES(1,1,'pickup','quest_item','test',0)")
        self.db.commit()

    def insert(self, **changes):
        row = dict(mod_id=1, base_id=100, quest_mod_id=1, quest_base_id=200,
                   alias_id=11, reference_mod_id=0, reference_base_id=0,
                   count=1, quest_object=1, active=1, revision=1, quest_instance=0)
        row.update(changes)
        self.db.execute("INSERT INTO quest_items VALUES(?,?,?,?,?,?,?,?,?,?,?,?)", tuple(row.values()))

    def test_additive_schema_is_repeatable(self):
        self.assertEqual(len(SCHEMA), 4)
        self.insert()
        for sql in SCHEMA:
            self.db.execute(sql)
        self.assertEqual(self.db.execute("SELECT count(*) FROM quest_items").fetchone(), (1,))

    def test_identity_includes_quest_and_alias(self):
        self.insert()
        self.insert(alias_id=12)
        self.insert(quest_base_id=201)
        self.insert(quest_instance=1)
        with self.assertRaises(sqlite3.IntegrityError):
            self.insert()

    def test_retired_rows_keep_identity(self):
        self.insert(active=0)
        with self.assertRaises(sqlite3.IntegrityError):
            self.insert(active=1)

    def test_invalid_quantity_and_flags_are_rejected(self):
        for values in ({"count": 0}, {"count": -1}, {"count": 65536},
                       {"active": 2}, {"quest_object": 2}):
            with self.subTest(values=values), self.assertRaises(sqlite3.IntegrityError):
                self.insert(**values)

    def test_item_requires_a_journal_revision(self):
        with self.assertRaises(sqlite3.IntegrityError):
            self.insert(revision=2)

    def test_failed_item_insert_rolls_back_journal(self):
        with self.assertRaises(sqlite3.IntegrityError), self.db:
            self.db.execute("INSERT INTO journal VALUES(2,1,'bad','quest_item','test',0)")
            self.insert(count=0, revision=2)
        self.assertEqual(self.db.execute("SELECT count(*) FROM journal").fetchone(), (1,))
        self.assertEqual(self.db.execute("SELECT count(*) FROM quest_items").fetchone(), (0,))


if __name__ == "__main__":
    unittest.main()
