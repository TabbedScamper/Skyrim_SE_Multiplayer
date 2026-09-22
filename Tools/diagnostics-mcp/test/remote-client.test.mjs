import assert from "node:assert/strict";
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { materializeRemoteBundle } from "../src/remote-client.mjs";

test("materializeRemoteBundle writes only the named capture triplet", async () => {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), "skyrim-follower-capture-"));
  const payload = {
    found: true,
    stem: "feedback-20260922-120000-42",
    capturedAt: "2026-09-22T12:00:00.000Z",
    files: [
      { name: "feedback-20260922-120000-42.game.json", bytes: 11, dataBase64: Buffer.from('{"ok":true}').toString("base64") },
      { name: "feedback-20260922-120000-42.txt", bytes: 4, dataBase64: Buffer.from("note").toString("base64") },
    ],
  };
  const result = await materializeRemoteBundle(payload, root);
  assert.equal(result.found, true);
  assert.equal(result.files.length, 2);
  assert.equal(
    await fs.readFile(path.join(result.directory, payload.files[0].name), "utf8"),
    '{"ok":true}',
  );
});

test("materializeRemoteBundle rejects path traversal", async () => {
  await assert.rejects(
    materializeRemoteBundle({ found: true, stem: "../escape", files: [] }, os.tmpdir()),
    /unsafe/,
  );
});
