import fs from "node:fs/promises";
import path from "node:path";

const logPattern = /^\[([^\]]+)] \[([^\]]+)] \[tid ([^\]]+)]\s+(.*)$/;

export async function ensureParent(filePath) {
  await fs.mkdir(path.dirname(filePath), { recursive: true });
}

export async function appendLine(filePath, line) {
  await ensureParent(filePath);
  await fs.appendFile(filePath, `${line}\n`, "utf8");
}

export async function readTail(filePath, maxBytes = 2 * 1024 * 1024) {
  let handle;
  try {
    handle = await fs.open(filePath, "r");
    const stats = await handle.stat();
    const bytesToRead = Math.min(stats.size, maxBytes);
    const buffer = Buffer.alloc(bytesToRead);
    await handle.read(buffer, 0, bytesToRead, stats.size - bytesToRead);
    return buffer.toString("utf8");
  } catch (error) {
    if (error?.code === "ENOENT") return "";
    throw error;
  } finally {
    await handle?.close();
  }
}

export function parseLogText(text, source) {
  return text
    .split(/\r?\n/)
    .map((line) => {
      const match = logPattern.exec(line);
      if (!match) return null;
      return {
        timestamp: match[1],
        severity: match[2].toLowerCase(),
        thread: match[3],
        source,
        message: match[4],
      };
    })
    .filter(Boolean);
}

export async function readJsonLines(filePath) {
  const text = await readTail(filePath);
  const values = [];
  for (const line of text.split(/\r?\n/)) {
    if (!line.trim()) continue;
    try {
      values.push(JSON.parse(line));
    } catch {
      // A writer may still be completing the final line. Ignore malformed data.
    }
  }
  return values;
}

export async function describeFile(filePath) {
  try {
    const stats = await fs.stat(filePath);
    return {
      path: filePath,
      exists: true,
      bytes: stats.size,
      modified: stats.mtime.toISOString(),
    };
  } catch (error) {
    if (error?.code === "ENOENT") return { path: filePath, exists: false };
    throw error;
  }
}

export function sanitizeCommandField(value) {
  return String(value).replace(/[\t\r\n]+/g, " ").trim();
}
