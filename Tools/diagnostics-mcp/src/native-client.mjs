import net from "node:net";
import fs from "node:fs/promises";
import path from "node:path";
import { nativePipe } from "./config.mjs";
import { runtimeDirectory } from "./config.mjs";

let nextRequestId = 0;

export function extractJsonLine(buffer) {
  const newline = buffer.indexOf("\n");
  if (newline < 0) return null;
  const line = buffer.slice(0, newline).replace(/\r$/, "");
  return { value: JSON.parse(line), remainder: buffer.slice(newline + 1) };
}

export function invokeNative(command, arguments_ = {}, timeoutMs = 5000) {
  return new Promise((resolve, reject) => {
    const request = { id: ++nextRequestId, command, ...arguments_ };
    const socket = net.createConnection(nativePipe);
    let buffer = "";
    let settled = false;

    const finish = (callback, value) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      socket.destroy();
      callback(value);
    };

    const timer = setTimeout(
      () => finish(reject, new Error(`Native bridge timed out running ${command}`)),
      timeoutMs,
    );

    socket.setEncoding("utf8");
    socket.on("connect", () => socket.write(`${JSON.stringify(request)}\n`));
    socket.on("data", (chunk) => {
      buffer += chunk;
      let parsed;
      try {
        parsed = extractJsonLine(buffer);
      } catch (error) {
        finish(reject, error);
        return;
      }
      if (!parsed) return;
      if (!parsed.value.ok) {
        finish(reject, new Error(parsed.value.error ?? `Native command ${command} failed`));
        return;
      }
      finish(resolve, parsed.value);
    });
    socket.on("error", (error) => finish(reject, error));
    socket.on("end", () => {
      if (!settled) finish(reject, new Error("Native bridge closed without a response"));
    });
  });
}

export async function getNativeStatus() {
  const ping = await invokeNative("ping");
  const capabilities = await invokeNative("capabilities");
  return { pipe: nativePipe, ping, capabilities };
}

export const getGameSnapshot = () => invokeNative("game_snapshot");
export const watchQuest = (editorId) =>
  invokeNative("watch_quest", { editorId });
export const captureClientBundle = () => invokeNative("capture_bundle", {}, 15000);

const sleep = (milliseconds) =>
  new Promise((resolve) => setTimeout(resolve, milliseconds));

export async function recordClientTimeline({ durationMs = 5000, intervalMs = 100 } = {}) {
  const startedAt = new Date();
  const deadline = Date.now() + durationMs;
  const samples = [];
  let previousFingerprint = "";
  let changeCount = 0;

  while (Date.now() <= deadline) {
    const response = await getGameSnapshot();
    const game = response.game ?? null;
    const fingerprint = JSON.stringify(game);
    if (fingerprint !== previousFingerprint) {
      changeCount += 1;
      previousFingerprint = fingerprint;
    }
    samples.push({ observedAt: new Date().toISOString(), ageMs: response.ageMs, game });
    if (Date.now() < deadline) await sleep(intervalMs);
  }

  await fs.mkdir(runtimeDirectory, { recursive: true });
  const stamp = startedAt.toISOString().replaceAll(":", "-").replaceAll(".", "-");
  const outputPath = path.join(runtimeDirectory, `client-timeline-${stamp}.json`);
  await fs.writeFile(
    outputPath,
    JSON.stringify({ startedAt: startedAt.toISOString(), durationMs, intervalMs, samples }, null, 2),
  );
  return { outputPath, sampleCount: samples.length, changeCount };
}
