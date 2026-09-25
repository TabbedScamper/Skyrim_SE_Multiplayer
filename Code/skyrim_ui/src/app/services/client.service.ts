import { Injectable, NgZone, OnDestroy } from '@angular/core';
import { TranslocoService } from '@ngneat/transloco';
import { AsyncSubject, BehaviorSubject, ReplaySubject, Subject } from 'rxjs';
import { environment } from '../../environments/environment';
import { Debug } from '../models/debug';
import { PartyInfo } from '../models/party-info';
import { Player } from '../models/player';
import { AudioDevice, DisplayMode, GameSettings, GameSettingsPayload } from '../models/game-settings';
import { ChatService } from './chat.service';
import { CoopLobbyState } from '../models/coop-lobby-state';
import { SteamLobbyState } from '../models/steam-lobby-state';
import { ErrorEvents, ErrorService } from './error.service';
import { LoadingService } from './loading.service';

/** Client game service. */
@Injectable({
  providedIn: 'root',
})
export class ClientService implements OnDestroy {
  /** Initialization done. */
  public initDone = new AsyncSubject<undefined>();

  /** Activation state change. */
  public activationStateChange = new BehaviorSubject(false);

  /** InGame state change. */
  public inGameStateChange = new BehaviorSubject(!environment.game);
  public titleScreenStateChange = new BehaviorSubject(false);
  public titleOptionsRequested = new Subject<void>();
  public titleLobbyRequested = new Subject<void>();
  public coopLobbyStateChange = new ReplaySubject<CoopLobbyState>(1);
  public coopGameplaySettingsChange = new ReplaySubject<{ difficulty: number; pvpEnabled: boolean; deathSystemEnabled: boolean; greetingsEnabled: boolean }>(1);
  public steamLobbyStateChange = new ReplaySubject<SteamLobbyState>(1);
  public deploymentScanStateChange = new ReplaySubject<{ complete: boolean; fileCount: number; hashed: number; cached: number; errors: number }>(1);

  /** Opening/close menu change. */
  public openingMenuChange = new BehaviorSubject(false);

  /** Connection state change. */
  public connectionStateChange = new BehaviorSubject(false);

  public isConnectionInProgressChange = new BehaviorSubject(false);

  /** Player name change. */
  public nameChange = new BehaviorSubject(environment.game ? '' : 'test');

  /** Client version setting. */
  public versionSet = new BehaviorSubject(environment.game ? '' : 'browser');

  /** Debug state change. */
  public debugStateChange = new Subject<boolean>();

  /** Debug data change. */
  public debugDataChange = new BehaviorSubject(new Debug());

  /** Connect player to server change. */
  public playerConnectedChange = new Subject<Player>();

  /** Connect party info change. */
  public partyInfoChange = new Subject<PartyInfo>();

  /** Connect party info change. */
  public partyLeftChange = new Subject<void>();

  /** Connect party invite received. */
  public partyInviteReceivedChange = new Subject<number>();

  /** Disconnect player to server change. */
  public playerDisconnectedChange = new Subject<Player>();

  /** Player health change. */
  public healthChange = new Subject<Player>();

  /** Player level change. */
  public levelChange = new Subject<Player>();

  /** Player cell change. */
  public cellChange = new Subject<Player>();

  /** Player isLoaded change. */
  public isLoadedChange = new Subject<Player>();

  public protocolMismatchChange = new BehaviorSubject(false);

  public gameSettingsChange = new ReplaySubject<GameSettingsPayload>(1);
  public displayPreviewStarted = new Subject<void>();
  public displayPreviewReverted = new Subject<void>();
  public gameSettingsApplied = new Subject<void>();
  public debugPrompt = new Subject<string>();
  public debugPromptCancelled = new Subject<void>();
  public debugPromptSubmitted = new Subject<void>();

  /** Receive error from core */
  public triggerError = new ReplaySubject<ErrorEvents>(1);

  /** Used purely for debugging. */
  public debugChange = new Subject<void>();

  // The below emitters are used in the mocking service

  /** Used for when a party leader changed. */
  public partyLaunchedChange = new Subject<void>();

  /** Used for when a party invite is sent. */
  public partyInviteChange = new Subject<number>();

  /** Used for when a party invite was accepted by the local player. */
  public partyJoinedChange = new Subject<number>();

  /** Used for when a party member was kicked. */
  public memberKickedChange = new Subject<number>();

  /** Used for when a party leader changed. */
  public partyLeaderChange = new Subject<number>();

  public localPlayerId = undefined;

  private _host: string;

  private _port: number;

  private _password: string;

  private _remainingReconnectionAttempt = environment.nbReconnectionAttempts;

  /**
   * Instantiate.
   */
  public constructor(
    private readonly zone: NgZone,
    private readonly errorService: ErrorService,
    private readonly loadingService: LoadingService,
    private readonly translocoService: TranslocoService,
    private readonly chatService: ChatService,
  ) {
    skyrimtogether.on('init', this.onInit.bind(this));
    skyrimtogether.on('activate', this.onActivate.bind(this));
    skyrimtogether.on('deactivate', this.onDeactivate.bind(this));
    skyrimtogether.on('enterGame', this.onEnterGame.bind(this));
    skyrimtogether.on('exitGame', this.onExitGame.bind(this));
    skyrimtogether.on('enterTitleScreen', () => this.zone.run(() => this.titleScreenStateChange.next(true)));
    skyrimtogether.on('exitTitleScreen', () => this.zone.run(() => this.titleScreenStateChange.next(false)));
    skyrimtogether.on('showTitleOptions', () => this.zone.run(() => this.titleOptionsRequested.next()));
    skyrimtogether.on('showTitleLobby', () => this.zone.run(() => this.titleLobbyRequested.next()));
    skyrimtogether.on('coopLobbyState', (playerIds, leaderId, readyPlayerIds, campaignMode, sessionState, startEpoch, checkpointId, lobbyOpen, passwordProtected) =>
      this.zone.run(() => this.coopLobbyStateChange.next({ playerIds, leaderId, readyPlayerIds, campaignMode, sessionState, startEpoch, checkpointId, lobbyOpen, passwordProtected })));
    skyrimtogether.on('coopGameplaySettings', (difficulty, pvpEnabled, deathSystemEnabled, greetingsEnabled) =>
      this.zone.run(() => this.coopGameplaySettingsChange.next({ difficulty, pvpEnabled, deathSystemEnabled, greetingsEnabled })));
    skyrimtogether.on('steamLobbyState', (lobbyId, ownerId, memberIds, memberNames, friendIds, friendNames, open, passwordProtected, waitingForPassword, isHost) =>
      this.zone.run(() => this.steamLobbyStateChange.next({ lobbyId, ownerId, memberIds, memberNames, friendIds, friendNames, open, passwordProtected, waitingForPassword, isHost })));
    skyrimtogether.on('deploymentScanState', (complete, fileCount, hashed, cached, errors) =>
      this.zone.run(() => this.deploymentScanStateChange.next({ complete, fileCount, hashed, cached, errors })));
    skyrimtogether.on('openingMenu', this.onOpeningMenu.bind(this));
    skyrimtogether.on('connect', this.onConnect.bind(this));
    skyrimtogether.on('disconnect', this.onDisconnect.bind(this));
    skyrimtogether.on('setName', this.onSetName.bind(this)); //not wanted, we dont sync name changes
    skyrimtogether.on('setVersion', this.onSetVersion.bind(this));
    skyrimtogether.on('debug', this.onDebug.bind(this)); //not needed anymore
    skyrimtogether.on('debugData', this.onUpdateDebug.bind(this));
    skyrimtogether.on('playerConnected', this.onPlayerConnected.bind(this));
    skyrimtogether.on(
      'playerDisconnected',
      this.onPlayerDisconnected.bind(this),
    );
    skyrimtogether.on('setHealth', this.onSetHealth.bind(this));
    skyrimtogether.on('setLevel', this.onSetLevel.bind(this));
    skyrimtogether.on('setCell', this.onSetCell.bind(this));
    skyrimtogether.on('setPlayer3dLoaded', this.onSetPlayer3dLoaded.bind(this));
    skyrimtogether.on(
      'setPlayer3dUnloaded',
      this.onSetPlayer3dUnloaded.bind(this),
    );
    skyrimtogether.on('setLocalPlayerId', this.onSetLocalPlayerId.bind(this));
    skyrimtogether.on('protocolMismatch', this.onProtocolMismatch.bind(this));
    skyrimtogether.on('triggerError', this.onTriggerError.bind(this));
    skyrimtogether.on('dummyData', this.onDummyData.bind(this));
    skyrimtogether.on('partyInfo', this.onPartyInfo.bind(this));
    skyrimtogether.on('partyCreated', this.onPartyCreated.bind(this));
    skyrimtogether.on('partyLeft', this.onPartyLeft.bind(this));
    skyrimtogether.on(
      'partyInviteReceived',
      this.onPartyInviteReceived.bind(this),
    );
    skyrimtogether.on('gameSettings', this.onGameSettings.bind(this));
    skyrimtogether.on('displayPreviewStarted', () => this.zone.run(() => this.displayPreviewStarted.next()));
    skyrimtogether.on('displayPreviewReverted', () => this.zone.run(() => this.displayPreviewReverted.next()));
    skyrimtogether.on('gameSettingsApplied', () => this.zone.run(() => this.gameSettingsApplied.next()));
    skyrimtogether.on('debugPrompt', (message: string, noteOnly: boolean) =>
      this.zone.run(() => this.debugPrompt.next(`${noteOnly ? 'report' : 'check'}\n${message}`)));
    skyrimtogether.on('cancelDebugPrompt', () =>
      this.zone.run(() => this.debugPromptCancelled.next()));
    skyrimtogether.on('submitDebugPrompt', () =>
      this.zone.run(() => this.debugPromptSubmitted.next()));
  }

  /**
   * Dispose.
   */
  public ngOnDestroy(): void {
    skyrimtogether.off('init');
    skyrimtogether.off('activate');
    skyrimtogether.off('deactivate');
    skyrimtogether.off('enterGame');
    skyrimtogether.off('exitGame');
    skyrimtogether.off('enterTitleScreen');
    skyrimtogether.off('exitTitleScreen');
    skyrimtogether.off('showTitleOptions');
    skyrimtogether.off('showTitleLobby');
    skyrimtogether.off('coopLobbyState');
    skyrimtogether.off('coopGameplaySettings');
    skyrimtogether.off('steamLobbyState');
    skyrimtogether.off('deploymentScanState');
    skyrimtogether.off('openingMenu');
    skyrimtogether.off('connect');
    skyrimtogether.off('disconnect');
    skyrimtogether.off('setName');
    skyrimtogether.off('setVersion');
    skyrimtogether.off('debug');
    skyrimtogether.off('debugData');
    skyrimtogether.off('playerConnected');
    skyrimtogether.off('playerDisconnected');
    skyrimtogether.off('setHealth');
    skyrimtogether.off('setLevel');
    skyrimtogether.off('setCell');
    skyrimtogether.off('setPlayer3dLoaded');
    skyrimtogether.off('setPlayer3dUnloaded');
    skyrimtogether.off('setLocalPlayerId');
    skyrimtogether.off('protocolMismatch');
    skyrimtogether.off('triggerError');
    skyrimtogether.off('dummyData');
    skyrimtogether.off('partyInfo');
    skyrimtogether.off('partyCreated');
    skyrimtogether.off('partyLeft');
    skyrimtogether.off('partyInviteReceived');
    skyrimtogether.off('gameSettings');
    skyrimtogether.off('displayPreviewStarted');
    skyrimtogether.off('displayPreviewReverted');
    skyrimtogether.off('gameSettingsApplied');
  }

  /**
   * Connect to a server at the given address and port.
   *
   * @param host IP address or hostname.
   * @param port Port.
   * @param password Password or admin password
   */
  public connect(host: string, port: number, password = ''): void {
    skyrimtogether.connect(host, port, password);
    this.isConnectionInProgressChange.next(true);
    this._host = host;
    this._port = port;
    this._password = password;
  }

  public hostSteamSession(): void {
    skyrimtogether.hostSteamSession();
  }

  public joinSteamSession(lobbyId: string): void {
    skyrimtogether.joinSteamSession(lobbyId);
  }

  public leaveSteamSession(): void {
    skyrimtogether.leaveSteamSession();
  }

  public joinSteamFriend(steamId: string): void {
    skyrimtogether.joinSteamFriend(steamId);
  }

  public inviteSteamFriend(): void {
    skyrimtogether.inviteSteamFriend();
  }

  public refreshSteamLobby(): void {
    skyrimtogether.refreshSteamLobby();
  }

  public setSteamSessionAccess(open: boolean, password = ''): void {
    skyrimtogether.setSteamSessionAccess(open, password);
  }

  public setCoopGameplaySettings(difficulty: number, pvpEnabled: boolean): void {
    skyrimtogether.setCoopGameplaySettings(difficulty, pvpEnabled);
  }

  public connectJoinedSteamSession(password = ''): void {
    skyrimtogether.connectJoinedSteamSession(password);
  }

  public requestGameSettings(): void {
    skyrimtogether.requestGameSettings();
  }

  public previewGameSetting(name: string, value: number | boolean | string): void {
    skyrimtogether.previewGameSetting(name, String(value));
  }

  public confirmDisplaySettings(): void {
    skyrimtogether.confirmDisplaySettings();
  }

  public applyGameSettings(settings: GameSettings): void {
    skyrimtogether.applyGameSettings(
      settings.displayMode, settings.monitor, settings.width, settings.height, settings.vsync,
      settings.master, settings.footsteps, settings.voice, settings.music, settings.effects,
      settings.gamma, settings.mouseSensitivity, settings.gamepadSensitivity, settings.invertY,
      settings.dialogueSubtitles, settings.generalSubtitles, settings.alwaysRun, settings.controllerRumble,
    );
  }

  public revertGameSettings(): void {
    skyrimtogether.revertGameSettings();
  }

  public resetGameSettings(): void {
    skyrimtogether.resetGameSettings();
  }

  public openTitleOptions(): void {
    skyrimtogether.openTitleOptions();
  }

  public openTitleLobby(): void {
    skyrimtogether.openTitleLobby();
  }

  public setPartyReady(ready: boolean): void {
    skyrimtogether.setPartyReady(ready);
  }

  public selectSharedCampaign(mode: number, checkpointId = ''): void {
    skyrimtogether.selectSharedCampaign(mode, checkpointId);
  }

  public startTogether(mode: number, checkpointId = ''): void {
    skyrimtogether.startTogether(mode, checkpointId);
  }

  public submitDebugFeedback(looksRight: boolean, note = ''): void {
    skyrimtogether.submitDebugFeedback(looksRight, note);
  }

  /**
   * Disconnect from the server or cancel connection.
   */
  public disconnect(): void {
    skyrimtogether.disconnect();
    this._remainingReconnectionAttempt = 0;
  }

  /**
   * Reveal players in the immediate area (lighting them up with a visual effect).
   */
  public revealPlayers(): void {
    skyrimtogether.revealPlayers();
  }

  /**
   * Launch a party.
   */
  public launchParty(): void {
    skyrimtogether.launchParty();
  }

  /**
   * Create a party invite.
   */
  public createPartyInvite(playerId: number): void {
    skyrimtogether.createPartyInvite(playerId);
  }

  /**
   * Accept a party invite.
   */
  public acceptPartyInvite(inviterId: number): void {
    skyrimtogether.acceptPartyInvite(inviterId);
  }

  /**
   * As a party leader, kick a player from the party.
   */
  public kickPartyMember(playerId: number): void {
    skyrimtogether.kickPartyMember(playerId);
  }

  /**
   * Leave a party.
   */
  public leaveParty(): void {
    skyrimtogether.leaveParty();
  }

  /**
   * As a party leader, make someone else the leader.
   */
  public changePartyLeader(playerId: number): void {
    skyrimtogether.changePartyLeader(playerId);
  }

  /**
   * Deactivate UI and release control.
   */
  public deactivate(): void {
    skyrimtogether.deactivate();
  }

  /**
   * Reconnect
   */
  public reconnect(): void {
    skyrimtogether.reconnect();
    this._remainingReconnectionAttempt = 0;
  }

  public teleportToPlayer(playerId: number): void {
    skyrimtogether.teleportToPlayer(playerId);
  }

  /**
   * Called when the UI is first initialized.
   */
  // TODO: is this still used?
  private onInit(): void {
    this.zone.run(() => {
      this.initDone.next(undefined);
      this.initDone.complete();
    });
  }

  /**
   * Called when the UI is activated.
   */
  private onActivate(): void {
    this.zone.run(() => {
      this.activationStateChange.next(true);
    });
  }

  /**
   * Called when the UI is deactivated.
   */
  private onDeactivate(): void {
    this.zone.run(() => {
      this.activationStateChange.next(false);
    });
  }

  /**
   * Called when the player enters a game.
   */
  private onEnterGame(): void {
    this.zone.run(() => {
      this.inGameStateChange.next(true);
    });
  }

  /**
   * Called when the player exits a game.
   */
  private onExitGame(): void {
    this.zone.run(() => {
      this.inGameStateChange.next(false);
    });
  }

  /**
   * Called when the player open/close a game menu
   * @param openingMenu
   */
  private onOpeningMenu(openingMenu: boolean): void {
    this.zone.run(() => {
      this.openingMenuChange.next(openingMenu);
    });
  }

  /**
   * Called when a connection is made.
   */
  private onConnect(): void {
    this.zone.run(async () => {
      this._remainingReconnectionAttempt = environment.nbReconnectionAttempts;
      this.isConnectionInProgressChange.next(false);
      this.connectionStateChange.next(true);

      this.chatService.pushSystemMessage('SERVICE.CLIENT.CONNECTED');
    });
  }

  /**
   * Called when a connection is terminated.
   */
  private onDisconnect(isError: boolean): void {
    void this.zone.run(async () => {
      this.localPlayerId = undefined;
      this.connectionStateChange.next(false);
      this.isConnectionInProgressChange.next(false);

      if (isError && this._remainingReconnectionAttempt > 0) {
        this._remainingReconnectionAttempt--;
        this.chatService.pushSystemMessage('SERVICE.CLIENT.CONNECTION_LOST');
        this.connect(this._host, this._port, this._password);
      } else {
        this.chatService.pushSystemMessage('SERVICE.CLIENT.DISCONNECTED');
      }
    });
  }

  /**
   * Called when the player's name changes.
   *
   * @param name Player's name.
   */
  private onSetName(name: string): void {
    this.zone.run(() => {
      this.nameChange.next(name);
    });
  }

  /**
   * Called when the client's version is set.
   *
   * @param version Game's version.
   */
  private onSetVersion(version: string): void {
    version = environment.overwriteVersion || version;

    this.zone.run(() => {
      this.versionSet.next(version);
    });
  }

  getVersion(): string {
    return this.versionSet.value;
  }

  /**
   * Add listener to when the player's press the F3 key
   */
  private onDebug(isDebug: boolean): void {
    this.zone.run(() => {
      this.debugStateChange.next(isDebug);
    });
  }

  private onUpdateDebug(
    numPacketsSent: number,
    numPacketsReceived: number,
    RTT: number,
    packetLoss: number,
    sentBandwidth: number,
    receivedBandwidth: number,
  ): void {
    this.zone.run(() => {
      this.debugDataChange.next(
        new Debug(
          numPacketsSent,
          numPacketsReceived,
          RTT,
          packetLoss,
          sentBandwidth,
          receivedBandwidth,
        ),
      );
    });
  }

  private onPlayerConnected(
    playerId: number,
    username: string,
    level: number,
    cellName: string,
  ) {
    if (environment.game) {
      console.log(
        `%conPlayerConnected`,
        'background: #009688; color: #fff; padding: 3px; font-size: 9px;',
        ...Array.from(arguments).map(v => JSON.stringify(v)),
      );
    }
    this.zone.run(() => {
      this.playerConnectedChange.next(
        new Player({
          name: username,
          id: playerId,
          connected: true,
          level: level,
          cellName: cellName,
        }),
      );
    });
  }

  private onPlayerDisconnected(playerId: number, username: string) {
    if (environment.game) {
      console.log(
        `%conPlayerDisconnected`,
        'background: #009688; color: #fff; padding: 3px; font-size: 9px;',
        ...Array.from(arguments).map(v => JSON.stringify(v)),
      );
    }
    this.zone.run(() => {
      this.playerDisconnectedChange.next(
        new Player({
          name: username,
          id: playerId,
          connected: false,
        }),
      );
    });
  }

  private onSetHealth(playerId: number, health: number) {
    this.zone.run(() => {
      this.healthChange.next(new Player({ id: playerId, health: health }));
    });
  }

  private onSetLevel(playerId: number, level: number) {
    if (environment.game) {
      console.log(
        `%conSetLevel`,
        'background: #009688; color: #fff; padding: 3px; font-size: 9px;',
        ...Array.from(arguments).map(v => JSON.stringify(v)),
      );
    }
    this.zone.run(() => {
      this.levelChange.next(new Player({ id: playerId, level: level }));
    });
  }

  private onSetCell(playerId: number, cellName: string) {
    if (environment.game) {
      console.log(
        `%conSetCell`,
        'background: #009688; color: #fff; padding: 3px; font-size: 9px;',
        ...Array.from(arguments).map(v => JSON.stringify(v)),
      );
    }
    this.zone.run(() => {
      this.cellChange.next(new Player({ id: playerId, cellName: cellName }));
    });
  }

  private onSetPlayer3dLoaded(playerId: number, health: number) {
    if (environment.game) {
      console.log(
        `%conSetPlayer3dLoaded`,
        'background: #009688; color: #fff; padding: 3px; font-size: 9px;',
        ...Array.from(arguments).map(v => JSON.stringify(v)),
      );
    }
    this.zone.run(() => {
      this.isLoadedChange.next(
        new Player({ id: playerId, isLoaded: true, health: health }),
      );
    });
  }

  private onSetPlayer3dUnloaded(playerId: number) {
    if (environment.game) {
      console.log(
        `%conSetPlayer3dUnloaded`,
        'background: #009688; color: #fff; padding: 3px; font-size: 9px;',
        ...Array.from(arguments).map(v => JSON.stringify(v)),
      );
    }
    this.zone.run(() => {
      this.isLoadedChange.next(new Player({ id: playerId, isLoaded: false }));
    });
  }

  private onSetLocalPlayerId(playerId: number) {
    if (environment.game) {
      console.log(
        `%conSetLocalPlayerId`,
        'background: #009688; color: #fff; padding: 3px; font-size: 9px;',
        ...Array.from(arguments).map(v => JSON.stringify(v)),
      );
    }
    this.zone.run(() => {
      this.localPlayerId = playerId;
    });
  }

  private onProtocolMismatch() {
    this.zone.run(() => {
      this.protocolMismatchChange.next(true);
    });
  }

  private onTriggerError(rawError: string) {
    this.zone.run(() => {
      const error = JSON.parse(rawError) as ErrorEvents;
      this.triggerError.next(error);
      void this.errorService.setError(error);
    });
  }

  private onDummyData(data: Array<number>) {
    this.zone.run(() => {
      this.debugChange.next();
      /*
      for (const numb of data) {
        console.log(numb);
      }
      console.log(data);
      */
    });
  }

  public onPartyInfo(playerIds: Array<number>, leaderId: number) {
    if (environment.game) {
      console.log(
        `%conPartyInfo`,
        'background: #009688; color: #fff; padding: 3px; font-size: 9px;',
        ...Array.from(arguments).map(v => JSON.stringify(v)),
      );
    }
    this.zone.run(() => {
      this.partyInfoChange.next(
        new PartyInfo({
          playerIds: playerIds,
          leaderId: leaderId,
        }),
      );
    });
  }

  private onPartyCreated() {
    if (environment.game) {
      console.log(
        `%conPartyCreated`,
        'background: #009688; color: #fff; padding: 3px; font-size: 9px;',
        ...Array.from(arguments).map(v => JSON.stringify(v)),
      );
    }
    this.zone.run(() => {
      this.loadingService.setLoading(false);
      this.partyInfoChange.next(
        new PartyInfo({
          playerIds: [this.localPlayerId],
          leaderId: this.localPlayerId,
        }),
      );
    });
  }

  private onPartyLeft() {
    if (environment.game) {
      console.log(
        `%conPartyLeft`,
        'background: #009688; color: #fff; padding: 3px; font-size: 9px;',
        ...Array.from(arguments).map(v => JSON.stringify(v)),
      );
    }
    this.zone.run(() => {
      this.partyLeftChange.next();
    });
  }

  private onPartyInviteReceived(inviterId: number) {
    if (environment.game) {
      console.log(
        `%conPartyInviteReceived`,
        'background: #009688; color: #fff; padding: 3px; font-size: 9px;',
        ...Array.from(arguments).map(v => JSON.stringify(v)),
      );
    }
    this.zone.run(() => {
      this.partyInviteReceivedChange.next(inviterId);
    });
  }

  private onGameSettings(
    displayMode: number, monitor: number, width: number, height: number, vsync: boolean,
    master: number, footsteps: number, voice: number, music: number, effects: number,
    gamma: number, mouseSensitivity: number, gamepadSensitivity: number, invertY: boolean,
    dialogueSubtitles: boolean, generalSubtitles: boolean, alwaysRun: boolean,
    controllerRumble: boolean, monitors: string, resolutions: string, defaults: boolean,
    audioDevice?: string, audioDevicesJson?: string,
  ): void {
    this.zone.run(() => this.gameSettingsChange.next({
      settings: {
        displayMode: displayMode as DisplayMode,
        monitor, width, height, vsync, master, footsteps, voice, music, effects,
        gamma, mouseSensitivity, gamepadSensitivity, invertY, dialogueSubtitles,
        generalSubtitles, alwaysRun, controllerRumble,
        audioDevice: audioDevice ?? '',
      },
      monitors: monitors ? monitors.split('|') : [],
      resolutions: resolutions ? resolutions.split('|') : [],
      audioDevices: ClientService.parseAudioDevices(audioDevicesJson),
      defaults,
    }));
  }

  /** Undefined when the game client predates audio device reporting. */
  private static parseAudioDevices(json?: string): AudioDevice[] | undefined {
    if (json === undefined) return undefined;
    try {
      const parsed = JSON.parse(json);
      return Array.isArray(parsed)
        ? parsed.filter(d => typeof d?.id === 'string' && typeof d?.name === 'string')
        : undefined;
    } catch {
      return undefined;
    }
  }
}
