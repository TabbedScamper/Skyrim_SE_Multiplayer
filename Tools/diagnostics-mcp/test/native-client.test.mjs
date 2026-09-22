import assert from "node:assert/strict";
import test from "node:test";
import { extractJsonLine } from "../src/native-client.mjs";

test("extractJsonLine waits for and parses one bridge response", () => {
  assert.equal(extractJsonLine('{"id":1'), null);
  assert.deepEqual(extractJsonLine('{"id":1,"ok":true}\r\nremaining'), {
    value: { id: 1, ok: true },
    remainder: "remaining",
  });
});
