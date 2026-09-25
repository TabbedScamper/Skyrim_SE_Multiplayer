/** A Steam friend who is not offline (SteamLobbyService::PublishLobbyState). */
export interface SteamFriend {
  id: string;
  name: string;
  /** coop = in a joinable co-op session, skyrim = playing Skyrim SE, online / away / busy. */
  status: 'coop' | 'skyrim' | 'online' | 'away' | 'busy';
  /** Their lobby when joinable. */
  lobby: string;
  /** Already in this lobby. */
  inLobby: boolean;
  /** Invited from this lobby already. */
  invited: boolean;
}

/** An invite received through Steam while the game is running. */
export interface SteamInvite {
  friendId: string;
  name: string;
  lobby: string;
}

export interface SteamLobbyState {
  lobbyId: string;
  ownerId: string;
  memberIds: string[];
  memberNames: string[];
  /** Friends in a joinable co-op lobby (kept for older callers; see friends). */
  friendIds: string[];
  friendNames: string[];
  open: boolean;
  passwordProtected: boolean;
  waitingForPassword: boolean;
  isHost: boolean;
  friends: SteamFriend[];
  offlineFriends: number;
  invites: SteamInvite[];
}
