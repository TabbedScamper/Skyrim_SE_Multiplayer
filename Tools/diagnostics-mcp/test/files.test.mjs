import assert from "node:assert/strict";
import test from "node:test";
import { parseLogText, sanitizeCommandField } from "../src/files.mjs";

test("parseLogText parses STR log records", () => {
  const entries = parseLogText(
    "[2026-09-20 22:42:39.213] [critical] [tid 57120]  VectoredExceptionHandler: crash occurred!",
    "leader",
  );

  assert.deepEqual(entries, [
    {
      timestamp: "2026-09-20 22:42:39.213",
      severity: "critical",
      thread: "57120",
      source: "leader",
      message: "VectoredExceptionHandler: crash occurred!",
    },
  ]);
});

test("sanitizeCommandField prevents mailbox line injection", () => {
  assert.equal(sanitizeCommandField("hello\tworld\r\nnext"), "hello world next");
});
