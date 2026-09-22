import { execFile } from "node:child_process";
import path from "node:path";
import { promisify } from "node:util";
import { projectRoot } from "./config.mjs";

const execFileAsync = promisify(execFile);

export async function runProtocolTests({ port = 12578, failOnKnownGaps = false } = {}) {
  const script = path.join(projectRoot, "Tools", "InGameTests", "Run-ProtocolScenarios.ps1");
  const args = [
    "-NoProfile",
    "-ExecutionPolicy",
    "Bypass",
    "-File",
    script,
    "-Port",
    String(port),
    "-Compact",
  ];
  if (failOnKnownGaps) args.push("-FailOnKnownGaps");

  try {
    const { stdout } = await execFileAsync("powershell.exe", args, {
      cwd: projectRoot,
      windowsHide: true,
      timeout: 30000,
      maxBuffer: 4 * 1024 * 1024,
    });
    return JSON.parse(stdout.trim());
  } catch (error) {
    const output = error.stdout?.trim();
    if (output) {
      try {
        return { ...JSON.parse(output), processExitCode: error.code };
      } catch {}
    }
    throw error;
  }
}
