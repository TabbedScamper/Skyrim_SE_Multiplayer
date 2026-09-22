import path from "node:path";
import { fileURLToPath } from "node:url";

const moduleDirectory = path.dirname(fileURLToPath(import.meta.url));

export const projectRoot = path.resolve(moduleDirectory, "../../..");

export const runtimeDirectory = path.resolve(
  process.env.SKYRIM_DIAGNOSTICS_RUNTIME ??
    path.join(projectRoot, "runtime", "diagnostics"),
);

export const logDirectory = path.resolve(
  process.env.SKYRIM_TOGETHER_LOG_DIR ??
    "C:/Program Files (x86)/Steam/steamapps/common/Skyrim Special Edition/Data/SkyrimTogetherReborn/logs",
);

export const commandFile = path.join(runtimeDirectory, "commands.tsv");
export const markerFile = path.join(runtimeDirectory, "markers.jsonl");
export const telemetryFile = path.join(runtimeDirectory, "telemetry.jsonl");
export const nativePipe =
  process.env.SKYRIM_DIAGNOSTICS_PIPE ??
  "\\\\.\\pipe\\SkyrimSEMultiplayer.Test";

export const followerSshHost =
  process.env.SKYRIM_FOLLOWER_SSH_HOST ?? "eflem@192.168.50.103";
export const followerSshKey = path.resolve(
  process.env.SKYRIM_FOLLOWER_SSH_KEY ??
    path.join(projectRoot, "runtime", "remote-access", "eriana_deploy_ed25519"),
);
export const followerKnownHosts = path.resolve(
  process.env.SKYRIM_FOLLOWER_KNOWN_HOSTS ??
    path.join(projectRoot, "runtime", "remote-access", "known_hosts"),
);
export const followerCaptureDirectory =
  process.env.SKYRIM_FOLLOWER_CAPTURE_DIR ??
  "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Skyrim Special Edition\\Data\\SkyrimTogetherReborn\\debug-feedback";
export const followerMirrorDirectory = path.join(
  runtimeDirectory,
  "follower-captures",
);

export const configuredLogs = [
  { source: "server", path: path.join(logDirectory, "STServerOut.log") },
  { source: "leader", path: path.join(logDirectory, "tp_client.log") },
  {
    source: "leader_previous",
    path: path.join(logDirectory, "tp_client.pre-clean-2259.log"),
  },
];
