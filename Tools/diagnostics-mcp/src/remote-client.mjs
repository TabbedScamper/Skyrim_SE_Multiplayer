import { execFile } from "node:child_process";
import fs from "node:fs/promises";
import path from "node:path";
import { promisify } from "node:util";
import {
  followerCaptureDirectory,
  followerKnownHosts,
  followerMirrorDirectory,
  followerSshHost,
  followerSshKey,
} from "./config.mjs";

const execFileAsync = promisify(execFile);

function encodePowerShell(command) {
  return Buffer.from(command, "utf16le").toString("base64");
}

function parseRemoteJson(stdout) {
  for (const line of stdout.trim().split(/\r?\n/).reverse()) {
    try {
      return JSON.parse(line);
    } catch {
      // OpenSSH/PowerShell may emit a banner before the JSON payload.
    }
  }
  throw new Error("Follower returned no valid capture metadata");
}

export async function materializeRemoteBundle(payload, targetRoot = followerMirrorDirectory) {
  if (!payload?.found) return payload ?? { found: false };
  if (!Array.isArray(payload.files) || !payload.stem) {
    throw new Error("Follower capture payload is incomplete");
  }

  const stem = path.basename(String(payload.stem));
  if (stem !== payload.stem) throw new Error("Follower capture stem is unsafe");
  const targetDirectory = path.join(targetRoot, stem);
  await fs.mkdir(targetDirectory, { recursive: true });

  const files = [];
  for (const entry of payload.files) {
    const name = path.basename(String(entry.name ?? ""));
    if (!name || name !== entry.name) throw new Error("Follower capture filename is unsafe");
    const destination = path.join(targetDirectory, name);
    await fs.writeFile(destination, Buffer.from(entry.dataBase64, "base64"));
    files.push({ path: destination, bytes: Number(entry.bytes ?? 0) });
  }
  return { found: true, stem, capturedAt: payload.capturedAt, directory: targetDirectory, files };
}

export async function pullLatestFollowerCapture() {
  const escapedDirectory = followerCaptureDirectory.replaceAll("'", "''");
  const command = [
    "$ErrorActionPreference='Stop'",
    "$ProgressPreference='SilentlyContinue'",
    "[Console]::OutputEncoding=[Text.Encoding]::UTF8",
    `$dir='${escapedDirectory}'`,
    "if (!(Test-Path -LiteralPath $dir)) { @{found=$false;reason='capture-directory-missing'} | ConvertTo-Json -Compress; exit 0 }",
    "$state=Get-ChildItem -LiteralPath $dir -File -Filter '*.game.json' | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1",
    "if (!$state) { @{found=$false;reason='no-f10-captures'} | ConvertTo-Json -Compress; exit 0 }",
    "$stem=$state.Name.Substring(0,$state.Name.Length-10)",
    "$wanted=@(\"$stem.bmp\",\"$stem.txt\",\"$stem.game.json\")",
    "$files=@(Get-ChildItem -LiteralPath $dir -File | Where-Object { $wanted -contains $_.Name } | ForEach-Object { @{name=$_.Name;bytes=$_.Length;dataBase64=[Convert]::ToBase64String([IO.File]::ReadAllBytes($_.FullName))} })",
    "@{found=$true;stem=$stem;capturedAt=$state.LastWriteTimeUtc.ToString('o');files=$files} | ConvertTo-Json -Depth 4 -Compress",
  ].join("; ");

  const ssh = process.env.WINDIR
    ? path.join(process.env.WINDIR, "System32", "OpenSSH", "ssh.exe")
    : "ssh";
  const { stdout } = await execFileAsync(
    ssh,
    [
      "-i", followerSshKey,
      "-o", `UserKnownHostsFile=${followerKnownHosts}`,
      "-o", "StrictHostKeyChecking=yes",
      "-o", "BatchMode=yes",
      "-o", "ConnectTimeout=5",
      followerSshHost,
      `powershell -NoProfile -NonInteractive -EncodedCommand ${encodePowerShell(command)}`,
    ],
    { windowsHide: true, maxBuffer: 64 * 1024 * 1024, timeout: 15000 },
  );
  return materializeRemoteBundle(parseRemoteJson(stdout));
}
