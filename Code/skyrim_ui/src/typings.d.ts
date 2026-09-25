declare const module: NodeModule;

interface NodeModule {
  id: string;
}

/** Skyim: Together type definitions */
declare namespace SkyrimTogetherTypes {
  /** Client initialization callback */
  type InitCallback = () => void;

  /** UI activation callback */
  type ActivateCallback = () => void;

  /** UI deactivation callback */
  type DeactivateCallback = () => void;

  /** Player game entry callback */
  type EnterGameCallback = () => void;

  /** Player game exit callback */
  type ExitGameCallback = () => void;
  type TitleScreenCallback = () => void;

  /** Player open/close game menu callback */
  type OpeningMenuCallback = (openingMenu: boolean) => void;

  /** Chat message reception callback */
  type MessageCallback = (
    type: number,
    content: string,
    sender: string,
  ) => void;

  /** Connection callback */
  type ConnectCallback = () => void;

  /** Disconnection callback */
  type DisconnectCallback = (isError: boolean) => void;

  /** Name change callback */
  type SetNameCallback = (name: string) => void;

  /** Version set callback */
  type SetVersionCallback = (version: string) => void;

  /**  */
  type OnDebugCallback = (isDebug: boolean) => void;

  type UpdateDebugCallback = (
    numPacketsSent: number,
    numPacketsReceived: number,
    RTT: number,
    packetLoss: number,
    sentBandwidth: number,
    receivedBandwidth: number,
  ) => void;

  type UserDataSetCallback = (password: string, username: string) => void;

  type PlayerConnectedCallback = (
    playerId: number,
    username: string,
    level: number,
    cellName: string,
  ) => void;

  type PlayerDisconnectedCallback = (
    playerId: number,
    username: string,
  ) => void;

  type SetHealthCallback = (playerId: number, health: number) => void;

  type SetLevelCallback = (playerId: number, level: number) => void;

  type SetCellCallback = (playerId: number, cellName: string) => void;

  type SetPlayer3dLoadedCallback = (playerId: number, health: number) => void;

  type SetPlayer3dUnloadedCallback = (playerId: number) => void;

  type SetLocalPlayerIdCallback = (playerId: number) => void;

  type ProtocolMismatch = () => void;

  type TriggerError = () => void;

  type DummyDataCallback = (data: Array<number>) => void;

  type PartyInfoCallback = (playerIds: Array<number>, leaderId: number) => void;
  type CoopLobbyStateCallback = (
    playerIds: Array<number>, leaderId: number, readyPlayerIds: Array<number>, campaignMode: number,
    sessionState: number, startEpoch: string, checkpointId: string, lobbyOpen: boolean, passwordProtected: boolean,
  ) => void;
  type CoopGameplaySettingsCallback = (difficulty: number, pvpEnabled: boolean, deathSystemEnabled: boolean, greetingsEnabled: boolean) => void;
  type SteamLobbyStateCallback = (
    lobbyId: string, ownerId: string, memberIds: string[], memberNames: string[], friendIds: string[], friendNames: string[],
    open: boolean, passwordProtected: boolean, waitingForPassword: boolean, isHost: boolean,
    /** JSON {friends, offline, invites} (see models/steam-lobby-state.ts). */
    social?: string,
  ) => void;
  type DeploymentScanStateCallback = (complete: boolean, fileCount: number, hashed: number, cached: number, errors: number) => void;

  type PartyCreatedCallback = () => void;

  type PartyLeftCallback = (inviterId: number) => void;

  type PartyInviteReceivedCallback = (inviterId: number) => void;

  type GameSettingsCallback = (...settings: Array<number | boolean | string>) => void;
  type DebugPromptCallback = (message: string, noteOnly: boolean) => void;
  type VoidCallback = () => void;
}

/** Global Skyrim: Together object. */
declare const skyrimtogether: SkyrimTogether;

/** Global Skyrim: Together object type. */
interface SkyrimTogether {
  /** Add listener to when the UI is first initialized. */
  on(event: 'init', callback: SkyrimTogetherTypes.InitCallback): void;

  on(event: 'debugPrompt', callback: SkyrimTogetherTypes.DebugPromptCallback): void;

  on(event: 'cancelDebugPrompt', callback: SkyrimTogetherTypes.VoidCallback): void;

  on(event: 'submitDebugPrompt', callback: SkyrimTogetherTypes.VoidCallback): void;

  /** Add listener to when the UI is activated. */
  on(event: 'activate', callback: SkyrimTogetherTypes.ActivateCallback): void;

  /** Add listener to when the UI is deactivated. */
  on(
    event: 'deactivate',
    callback: SkyrimTogetherTypes.DeactivateCallback,
  ): void;

  /** Add listener to when the player enters a game. */
  on(event: 'enterGame', callback: SkyrimTogetherTypes.EnterGameCallback): void;

  /** Add listener to when the player exits a game. */
  on(event: 'exitGame', callback: SkyrimTogetherTypes.ExitGameCallback): void;
  on(event: 'enterTitleScreen', callback: SkyrimTogetherTypes.TitleScreenCallback): void;
  on(event: 'exitTitleScreen', callback: SkyrimTogetherTypes.TitleScreenCallback): void;
  on(event: 'showTitleOptions', callback: SkyrimTogetherTypes.VoidCallback): void;

  /** Add listener to when the player open/close a game menu. */
  on(
    event: 'openingMenu',
    callback: SkyrimTogetherTypes.OpeningMenuCallback,
  ): void;

  /** Add listener to when a player message is received. */
  on(event: 'message', callback: SkyrimTogetherTypes.MessageCallback): void;

  /** Add listener to when the player connects to a server. */
  on(event: 'connect', callback: SkyrimTogetherTypes.ConnectCallback): void;

  /** Add listener to when the player disconnects from a server. */
  on(
    event: 'disconnect',
    callback: SkyrimTogetherTypes.DisconnectCallback,
  ): void;

  /** Add listener to when the player's name changes. */
  on(event: 'setName', callback: SkyrimTogetherTypes.SetNameCallback): void;

  /** Add listener to when the client's version is set. */
  on(
    event: 'setVersion',
    callback: SkyrimTogetherTypes.SetVersionCallback,
  ): void;

  /** Add listener to when the player's press the F3 key */
  on(event: 'debug', callback: SkyrimTogetherTypes.OnDebugCallback): void;

  on(
    event: 'debugData',
    callback: SkyrimTogetherTypes.UpdateDebugCallback,
  ): void;

  /** Add listener to when the player's is connected with the launcher */
  on(
    event: 'userDataSet',
    callback: SkyrimTogetherTypes.UserDataSetCallback,
  ): void;

  /** Add listener to when one player connect in server. */
  on(
    event: 'playerConnected',
    callback: SkyrimTogetherTypes.PlayerConnectedCallback,
  ): void;

  /** Add listener to when one player disconnect in server. */
  on(
    event: 'playerDisconnected',
    callback: SkyrimTogetherTypes.PlayerDisconnectedCallback,
  ): void;

  on(event: 'setHealth', callback: SkyrimTogetherTypes.SetHealthCallback): void;

  /** Add listener to when one player change level in server. */
  on(event: 'setLevel', callback: SkyrimTogetherTypes.SetLevelCallback): void;

  /** Add listener to when one player change cell in server. */
  on(event: 'setCell', callback: SkyrimTogetherTypes.SetCellCallback): void;

  /** Add listener to when a player is loaded or unloaded in 3D.  */
  on(
    event: 'setPlayer3dLoaded',
    callback: SkyrimTogetherTypes.SetPlayer3dLoadedCallback,
  ): void;

  on(
    event: 'setPlayer3dUnloaded',
    callback: SkyrimTogetherTypes.SetPlayer3dUnloadedCallback,
  ): void;

  on(
    event: 'setLocalPlayerId',
    callback: SkyrimTogetherTypes.SetLocalPlayerIdCallback,
  ): void;

  on(
    event: 'protocolMismatch',
    callback: SkyrimTogetherTypes.ProtocolMismatch,
  ): void;

  on(event: 'triggerError', callback: SkyrimTogetherTypes.TriggerError): void;

  on(event: 'dummyData', callback: SkyrimTogetherTypes.DummyDataCallback): void;

  on(event: 'partyInfo', callback: SkyrimTogetherTypes.PartyInfoCallback): void;
  on(event: 'coopLobbyState', callback: SkyrimTogetherTypes.CoopLobbyStateCallback): void;
  on(event: 'coopGameplaySettings', callback: SkyrimTogetherTypes.CoopGameplaySettingsCallback): void;
  on(event: 'showTitleLobby', callback: SkyrimTogetherTypes.VoidCallback): void;
  on(event: 'steamLobbyState', callback: SkyrimTogetherTypes.SteamLobbyStateCallback): void;
  on(event: 'steamAvatar', callback: (steamId: string, dataUrl: string) => void): void;
  on(event: 'deploymentScanState', callback: SkyrimTogetherTypes.DeploymentScanStateCallback): void;

  on(
    event: 'partyCreated',
    callback: SkyrimTogetherTypes.PartyCreatedCallback,
  ): void;

  on(event: 'partyLeft', callback: SkyrimTogetherTypes.PartyLeftCallback): void;

  on(
    event: 'partyInviteReceived',
    callback: SkyrimTogetherTypes.PartyInviteReceivedCallback,
  ): void;

  on(event: 'gameSettings', callback: SkyrimTogetherTypes.GameSettingsCallback): void;
  on(event: 'controlBindings', callback: (json: string) => void): void;
  on(event: 'displayPreviewStarted', callback: SkyrimTogetherTypes.VoidCallback): void;
  on(event: 'displayPreviewReverted', callback: SkyrimTogetherTypes.VoidCallback): void;
  on(event: 'gameSettingsApplied', callback: SkyrimTogetherTypes.VoidCallback): void;
  on(event: 'gamepadInput', callback: (action: string, repeat: boolean) => void): void;
  on(event: 'gamepadScroll', callback: (amount: number) => void): void;

  /** Remove listener from when the application is first initialized. */
  off(event: 'init', callback?: SkyrimTogetherTypes.InitCallback): void;

  /** Remove listener from when the UI is activated. */
  off(event: 'activate', callback?: SkyrimTogetherTypes.ActivateCallback): void;

  /** Remove listener from when the UI is deactivated. */
  off(
    event: 'deactivate',
    callback?: SkyrimTogetherTypes.DeactivateCallback,
  ): void;

  /** Remove listener from when the player enters a game. */
  off(
    event: 'enterGame',
    callback?: SkyrimTogetherTypes.EnterGameCallback,
  ): void;

  /** Remove listener from when the player exits a game. */
  off(event: 'exitGame', callback?: SkyrimTogetherTypes.ExitGameCallback): void;
  off(event: 'enterTitleScreen', callback?: SkyrimTogetherTypes.TitleScreenCallback): void;
  off(event: 'exitTitleScreen', callback?: SkyrimTogetherTypes.TitleScreenCallback): void;
  off(event: 'showTitleOptions', callback?: SkyrimTogetherTypes.VoidCallback): void;
  off(event: 'showTitleLobby', callback?: SkyrimTogetherTypes.VoidCallback): void;
  off(event: 'steamLobbyState', callback?: SkyrimTogetherTypes.SteamLobbyStateCallback): void;
  off(event: 'steamAvatar', callback?: (steamId: string, dataUrl: string) => void): void;
  off(event: 'deploymentScanState', callback?: SkyrimTogetherTypes.DeploymentScanStateCallback): void;

  /** Add listener to when the player open/close a game menu. */
  off(
    event: 'openingMenu',
    callback?: SkyrimTogetherTypes.OpeningMenuCallback,
  ): void;

  /** Remove listener from when a player message is received. */
  off(event: 'message', callback?: SkyrimTogetherTypes.MessageCallback): void;

  /** Remove listener from when the player connects to a server. */
  off(event: 'connect', callback?: SkyrimTogetherTypes.ConnectCallback): void;

  /** Remove listener from when the player disconnects from a server. */
  off(
    event: 'disconnect',
    callback?: SkyrimTogetherTypes.DisconnectCallback,
  ): void;

  /** Remove listener from when the player's name changes. */
  off(event: 'setName', callback?: SkyrimTogetherTypes.SetNameCallback): void;

  /** Remove listener from when the client's version is set. */
  off(
    event: 'setVersion',
    callback?: SkyrimTogetherTypes.SetVersionCallback,
  ): void;

  /** Remove listener to when the player's press the F3 key */
  off(event: 'debug', callback?: SkyrimTogetherTypes.OnDebugCallback): void;

  off(
    event: 'debugData',
    callback?: SkyrimTogetherTypes.UpdateDebugCallback,
  ): void;

  /** Add listener to when one player connect in server. */
  off(
    event: 'playerConnected',
    callback?: SkyrimTogetherTypes.PlayerConnectedCallback,
  ): void;

  /** Add listener to when one player disconnect in server. */
  off(
    event: 'playerDisconnected',
    callback?: SkyrimTogetherTypes.PlayerDisconnectedCallback,
  ): void;

  off(
    event: 'userDataSet',
    callback?: SkyrimTogetherTypes.UserDataSetCallback,
  ): void;

  off(
    event: 'setHealth',
    callback?: SkyrimTogetherTypes.SetHealthCallback,
  ): void;

  off(event: 'setLevel', callback?: SkyrimTogetherTypes.SetLevelCallback): void;

  off(event: 'setCell', callback?: SkyrimTogetherTypes.SetCellCallback): void;

  /** Add listener to when a player is loaded or unloaded in 3D.  */
  off(
    event: 'setPlayer3dLoaded',
    callback?: SkyrimTogetherTypes.SetPlayer3dLoadedCallback,
  ): void;

  off(
    event: 'setPlayer3dUnloaded',
    callback?: SkyrimTogetherTypes.SetPlayer3dUnloadedCallback,
  ): void;

  off(
    event: 'setLocalPlayerId',
    callback?: SkyrimTogetherTypes.SetLocalPlayerIdCallback,
  ): void;

  off(
    event: 'protocolMismatch',
    callback?: SkyrimTogetherTypes.ProtocolMismatch,
  ): void;

  off(event: 'triggerError', callback?: SkyrimTogetherTypes.TriggerError): void;

  off(
    event: 'dummyData',
    callback?: SkyrimTogetherTypes.DummyDataCallback,
  ): void;

  off(
    event: 'partyInfo',
    callback?: SkyrimTogetherTypes.PartyInfoCallback,
  ): void;
  off(event: 'coopLobbyState', callback?: SkyrimTogetherTypes.CoopLobbyStateCallback): void;
  off(event: 'coopGameplaySettings', callback?: SkyrimTogetherTypes.CoopGameplaySettingsCallback): void;

  off(
    event: 'partyCreated',
    callback?: SkyrimTogetherTypes.PartyCreatedCallback,
  ): void;

  off(
    event: 'partyLeft',
    callback?: SkyrimTogetherTypes.PartyLeftCallback,
  ): void;

  off(
    event: 'partyInviteReceived',
    callback?: SkyrimTogetherTypes.PartyInviteReceivedCallback,
  ): void;

  off(event: 'gameSettings', callback?: SkyrimTogetherTypes.GameSettingsCallback): void;
  off(event: 'controlBindings', callback?: (json: string) => void): void;
  off(event: 'displayPreviewStarted', callback?: SkyrimTogetherTypes.VoidCallback): void;
  off(event: 'displayPreviewReverted', callback?: SkyrimTogetherTypes.VoidCallback): void;
  off(event: 'gameSettingsApplied', callback?: SkyrimTogetherTypes.VoidCallback): void;
  off(event: 'gamepadInput', callback?: (action: string, repeat: boolean) => void): void;
  off(event: 'gamepadScroll', callback?: (amount: number) => void): void;

  /**
   * Connect to server at given address and port.
   *
   * @param host IP address or hostname.
   * @param port Port.
   * @param password Server password.
   */
  connect(host: string, port: number, password: string): void;

  /**
   * Disconnect from server or cancel connection.
   */
  disconnect(): void;

  /**
   * Reveal other players in the immediate area.
   */
  revealPlayers(): void;

  /**
   * Send message to server.
   */
  sendMessage(type: number, message: string): void;

  /**
   * Send a request to the server for changing the in-game time.
   */
  setTime(hours: number, minutes: number): void;

  /**
   * Deactivate UI and release control.
   */
  deactivate(): void;

  /**
   * Teleport to given player
   *
   * @param playerId Id of the player to which the requester should be teleported to
   */
  teleportToPlayer(playerId: number): void;

  /**
   * Reconnect the client.
   */
  reconnect(): void;

  /**
   * Launch a party.
   */
  launchParty(): void;

  setPartyReady(ready: boolean): void;

  selectSharedCampaign(mode: number, checkpointId: string): void;

  startTogether(mode: number, checkpointId: string): void;

  hostSteamSession(): void;

  joinSteamSession(lobbyId: string): void;

  leaveSteamSession(): void;

  joinSteamFriend(steamId: string): void;

  /** With a Steam id: a direct invite. Without: Steam's own invite dialog. */
  inviteSteamFriend(steamId?: string): void;
  answerSteamInvite(lobbyId: string, accept: boolean): void;

  refreshSteamLobby(): void;

  setSteamSessionAccess(open: boolean, password: string): void;
  setCoopGameplaySettings(difficulty: number, pvpEnabled: boolean): void;

  connectJoinedSteamSession(password: string): void;

  requestGameSettings(): void;
  requestControlBindings(): void;
  startControlCapture(event: string, device: number): void;
  cancelControlCapture(): void;
  audioPreviewKeepAlive(channel: string): void;
  audioPreviewStop(): void;

  previewGameSetting(name: string, value: string): void;

  confirmDisplaySettings(): void;

  applyGameSettings(...settings: Array<number | boolean>): void;

  revertGameSettings(): void;

  resetGameSettings(section?: string): void;

  openTitleOptions(): void;

  openTitleLobby(): void;

  submitDebugFeedback(looksRight: boolean, note: string): void;

  /**
   * Send a party invite to player with player id.
   *
   * @param playerId Id of the player to which the invite should be sent.
   */
  createPartyInvite(playerId: number): void;

  /**
   * Accept a party invite.
   *
   * @param inviterId Id of the player who sent the invite.
   */
  acceptPartyInvite(inviterId: number): void;

  /**
   * As a party leader, kick a member from the party.
   *
   * @param playerId Id of the player who gets kicked.
   */
  kickPartyMember(playerId: number): void;

  /**
   * Leave the currently joined party.
   */
  leaveParty(): void;

  /**
   * As a party leader, make someone else the leader.
   *
   * @param playerId Id of the new leader.
   */
  changePartyLeader(playerId: number): void;
}
