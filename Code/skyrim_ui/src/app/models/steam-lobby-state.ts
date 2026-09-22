export interface SteamLobbyState {
  lobbyId: string;
  ownerId: string;
  memberIds: string[];
  memberNames: string[];
  friendIds: string[];
  friendNames: string[];
  open: boolean;
  passwordProtected: boolean;
  waitingForPassword: boolean;
  isHost: boolean;
}
