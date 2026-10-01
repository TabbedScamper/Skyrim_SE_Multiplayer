export interface CoopLobbyState {
  playerIds: number[];
  leaderId: number;
  readyPlayerIds: number[];
  campaignMode: number;
  sessionState: number;
  startEpoch: string;
  checkpointId: string;
  lobbyOpen: boolean;
  passwordProtected: boolean;
}

/** One shared checkpoint on this PC, read from its save header. */
export interface CheckpointEntry {
  id: string;
  player: string;
  level: number;
  location: string;
  gameDate: string;
  savedMs: number;
  /** The save's own screenshot as a data URL, or empty. */
  image: string;
}

/** A character this PC can join a running session with (its newest .snap beside a save). */
export interface JoinCharacter {
  path: string;
  name: string;
  level: number;
  location: string;
  savedMs: number;
  image: string;
}
