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
