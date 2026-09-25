import { createStore } from '@ngneat/elf';
import {
  addEntities,
  deleteEntities,
  getAllEntities,
  getEntity,
  selectAllEntities,
  updateEntities,
  withEntities,
} from '@ngneat/elf-entities';
import { EventEmitter } from 'events';
import { fromEvent } from 'rxjs';
import { MessageTypes } from '../services/chat.service';
import { ErrorEvents } from '../services/error.service';
import { MockPlayer } from './mock-player';

let nextPlayerId = 1;

const playerStore = createStore(
  { name: 'players' },
  withEntities<MockPlayer>(),
);

export class SkyrimtogetherMock extends EventEmitter implements SkyrimTogether {
  private connected = false;
  private active = false;
  private version = 'browser';
  private playerName = 'Local Player';
  private showEvents = true;
  private localPlayerId: number;
  public readonly players$ = playerStore.pipe(selectAllEntities());

  connect(host: string, port: number, password: string): void {
    if (!this.connected) {
      let error: ErrorEvents | boolean;
      switch (host) {
        case 't-port':
        case 't-host':
          error = true;
          break;
        case 't-password':
          if (password !== 'test') {
            error = { error: 'wrong_password' };
          }
          break;
        case 't-version':
          error = {
            error: 'wrong_version',
            data: {
              version: '[current-version]',
              expectedVersion: '[expected-version]',
            },
          };
          break;
        case 't-full':
          error = { error: 'server_full' };
          break;
        case 't-client-mods':
          error = { error: 'client_mods_disallowed', data: { mods: ['SKSE'] } };
          break;
        case 't-mods':
          error = {
            error: 'mods_mismatch',
            data: {
              mods: [
                ['missing.esp', '0', 1],
                ['remove.esp', '12', 2],
                ['wrong_version.esp', '4', 32],
                ['wrong_order.esp', '9', 8],
              ],
            },
          };
          break;
      }
      setTimeout(() => {
        this.emit(!!error ? 'disconnect' : 'connect');
        this.connected = !error;
        if (error && typeof error !== 'boolean') {
          this.emit('triggerError', JSON.stringify(error));
        } else {
          this.emit('setLocalPlayerId', (this.localPlayerId = nextPlayerId++));
        }
        for (const player of playerStore.query(getAllEntities())) {
          this.emit(
            'playerConnected',
            player.id,
            player.name,
            player.level,
            player.cellName,
          );
          this.emit('setPlayer3dLoaded', player.id, player.health);
        }
      }, 250);
    }
  }

  disconnect(): void {
    if (this.connected) {
      this.emit('disconnect');
      this.connected = false;
    }
  }

  reconnect(): void {
    throw new Error('NOT YET IMPLEMENTED');
  }

  revealPlayers(): void {
    this.sendMessage(MessageTypes.SYSTEM_MESSAGE, "Revealing players...");
  }

  setTime(hours: number, minutes: number): void {
    this.sendMessage(MessageTypes.SYSTEM_MESSAGE, `Setting time to "${hours}:${minutes}"!`);
  }

  sendMessage(type: MessageTypes, message: string): void {
    if (this.connected) {
      this.emit('message', type, message, this.playerName);
    }
  }

  deactivate(): void {
    throw new Error('NOT YET IMPLEMENTED');
  }

  teleportToPlayer(playerId: number): void {
    if (this.connected) {
      const player = playerStore.query(getEntity(playerId));
      if (player) {
        console.log(
          `%cTELEPORT`,
          'background: #F09688; color: #fff; padding: 3px; font-size: 9px;',
          'Teleport to player',
          JSON.stringify(player.name),
          `(${player.id})`,
          'in',
          JSON.stringify(player.cellName),
        );
      }
    }
  }

  launchParty(): void {
    if (this.connected) {
      this.emit('partyCreated');
    }
  }

  hostSteamSession(): void {}

  joinSteamSession(_lobbyId: string): void {}

  leaveSteamSession(): void {}

  joinSteamFriend(_steamId: string): void {}

  inviteSteamFriend(): void {}

  refreshSteamLobby(): void {
    this.emit('steamLobbyState', '', '', [], [], [], [], false, false, false, false);
  }

  setSteamSessionAccess(_open: boolean, _password: string): void {}

  setCoopGameplaySettings(difficulty: number, pvpEnabled: boolean): void {
    this.emit('coopGameplaySettings', difficulty, pvpEnabled, true, false);
  }

  connectJoinedSteamSession(_password: string): void {}

  requestGameSettings(): void {
    this.emit('gameSettings', 1, 0, 1920, 1080, true, 1, 1, 0.8, 0.8, 1,
      1, 0.0125, 0.6667, false, true, true, true, true,
      'Display 1 (1920x1080)', '1280x720|1600x900|1920x1080|2560x1440|3840x2160', false,
      '', JSON.stringify([
        { id: '{0.0.0.00000000}.{mock-speakers}', name: 'Speakers (Realtek(R) Audio)' },
        { id: '{0.0.0.00000000}.{mock-headset}', name: 'Headset Earphone (Arctis Nova 7)' },
      ]));
  }

  previewGameSetting(_name: string, _value: string): void {}

  submitDebugFeedback(_looksRight: boolean, _note: string): void {}

  private mockBindings = [
    ['Forward', 0, 0x11], ['Back', 0, 0x1f], ['Strafe Left', 0, 0x1e], ['Strafe Right', 0, 0x20],
    ['Activate', 0, 0x12], ['Ready Weapon', 0, 0x13], ['Jump', 0, 0x39], ['Sprint', 0, 0x38],
    ['Sneak', 0, 0x1d], ['Shout', 0, 0x2c], ['Toggle POV', 0, 0x21], ['Tween Menu', 0, 0x0f],
    ['Wait', 0, 0x14], ['Journal', 0, 0x24], ['Quick Map', 0, 0x32], ['Favorites', 0, 0x10],
    ['Left Attack/Block', 1, 1], ['Right Attack/Block', 1, 0],
    ['Activate', 2, 0x1000], ['Tween Menu', 2, 0x2000], ['Ready Weapon', 2, 0x4000], ['Jump', 2, 0x8000],
    ['Shout', 2, 0x0200], ['Sprint', 2, 0x0100], ['Left Attack/Block', 2, 9], ['Right Attack/Block', 2, 10],
    ['Sneak', 2, 0x0040], ['Toggle POV', 2, 0x0080], ['Favorites', 2, 0x0001], ['Journal', 2, 0x0010],
    ['Wait', 2, 0x0020], ['Hotkey1', 2, 0x0004], ['Hotkey2', 2, 0x0008],
  ] as [string, number, number][];
  private captureEvent?: { event: string; device: number };

  requestControlBindings(): void {
    this.emit('controlBindings', JSON.stringify({
      controller: 'xbox',
      bindings: this.mockBindings.map(([event, device, key]) => ({ event, device, key, remappable: true })),
    }));
  }

  startControlCapture(event: string, device: number): void {
    this.captureEvent = { event, device };
  }

  cancelControlCapture(): void {
    this.captureEvent = undefined;
    this.requestControlBindings();
  }

  confirmDisplaySettings(): void {
    this.emit('gameSettingsApplied');
  }

  applyGameSettings(..._settings: Array<number | boolean>): void {
    this.emit('gameSettingsApplied');
  }

  revertGameSettings(): void {
    this.requestGameSettings();
  }

  resetGameSettings(): void {
    this.requestGameSettings();
  }

  openTitleOptions(): void {
    this.active = true;
    this.emit('activate');
  }

  openTitleLobby(): void {
    this.emit('showTitleLobby');
  }

  setPartyReady(_ready: boolean): void {}

  selectSharedCampaign(_mode: number, _checkpointId: string): void {}

  startTogether(_mode: number, _checkpointId: string): void {}

  createPartyInvite(playerId: number): void {
    playerStore.update(updateEntities(playerId, { invited: true }));
  }

  acceptPartyInvite(inviterId: number): void {
    this.emit('partyCreated');
    this.emit(
      'partyInfo',
      [
        ...playerStore
          .query(getAllEntities())
          .filter(p => p.isInGroup)
          .map(p => p.id),
      ],
      inviterId,
    );
  }

  kickPartyMember(playerId: number): void {
    playerStore.update(updateEntities(playerId, { isInGroup: false }));
    this.emit(
      'partyInfo',
      playerStore
        .query(getAllEntities())
        .filter(p => p.isInGroup)
        .map(p => p.id),
      this.localPlayerId,
    );
  }

  leaveParty(): void {
    if (this.connected) {
      this.emit('partyInfo', [], -1);
      this.emit('partyLeft');
    }
  }

  changePartyLeader(playerId: number): void {
    playerStore.update(updateEntities(playerId, { hasOwnParty: true }));
    this.emit(
      'partyInfo',
      [
        ...playerStore
          .query(getAllEntities())
          .filter(p => p.isInGroup)
          .map(p => p.id),
      ],
      playerId,
    );
  }

  initMock() {
    Object.keys((this as any)._events).forEach(e => {
      this.on(e, (...params) => {
        if (this.showEvents) {
          const eventName = e
            .replace(/(\G(?!^)|\b[a-zA-Z][a-z]*)([A-Z][a-z]*|\d+)/gm, `$1_$2`)
            .toUpperCase();
          console.log(
            `%cEVENT`,
            'background: #009688; color: #fff; padding: 3px; font-size: 9px;',
            `[${eventName}]`,
            ...params.map(v => JSON.stringify(v)),
          );
        }
      });
    });
    this.emit('init');
    this.emit('enterGame');
    this.emit('setVersion', this.version);
    this.emit('setName', this.playerName);

    fromEvent(window, 'keydown').subscribe((event: KeyboardEvent) => {
      if (event.ctrlKey && event.location === 2) {
        this.active = !this.active;
        this.emit(this.active ? 'activate' : 'deactivate');
        event.preventDefault();
        return;
      }
      switch (event.key) {
        case 'F2': {
          this.active = !this.active;
          this.emit(this.active ? 'activate' : 'deactivate');
          event.preventDefault();
          break;
        }
      }
    });
  }

  setMockVersion(version: string): void {
    this.version = version;
  }

  setMockPlayerName(name: string): void {
    this.playerName = name;
  }

  setShowEvents(show: boolean): void {
    this.showEvents = show;
  }

  addMockPlayer() {
    const newPlayerId = nextPlayerId++;
    const cities = [
      'Whiterun',
      'Dawnstar',
      'Falkreath',
      'Markarth',
      'Morthal',
      'Riften',
      'Solitude',
      'Windhelm',
    ];
    const newPlayer: MockPlayer = {
      name: 'Player ' + newPlayerId,
      id: newPlayerId,
      level: Math.floor(Math.random() * 100),
      health: Math.floor(Math.random() * 100),
      cellName: cities[Math.floor(Math.random() * cities.length)],
      hasOwnParty: false,
      isInGroup: false,
      invited: false,
      invitedLocalPlayer: false,
    };
    playerStore.update(addEntities(newPlayer));
    if (this.connected) {
      this.emit(
        'playerConnected',
        newPlayer.id,
        newPlayer.name,
        newPlayer.level,
        newPlayer.cellName,
      );
      this.emit('setPlayer3dLoaded', newPlayer.id, newPlayer.health);
    }
    // this.emit('setHealth', newPlayer.id, newPlayer.health);
    return newPlayer;
  }

  disconnectMockPlayer(playerId: number) {
    const mockPlayer = playerStore.query(getEntity(playerId));
    if (mockPlayer) {
      if (this.connected) {
        this.emit('playerDisconnected', mockPlayer.id, mockPlayer.name);
      }
      playerStore.update(deleteEntities(mockPlayer.id));
    }
  }

  accteptMockPlayerInvite(playerId: number) {
    playerStore.update(
      updateEntities(playerId, { invited: false, isInGroup: true }),
    );
    this.emit(
      'partyInfo',
      playerStore
        .query(getAllEntities())
        .filter(p => p.isInGroup)
        .map(p => p.id),
      this.localPlayerId,
    );
  }

  inviteToPlayerMockParty(playerId: number) {
    playerStore.update(updateEntities(playerId, { invitedLocalPlayer: true }));
    this.emit('partyInviteReceived', playerId);
  }

  startPlayerMockParty(playerId: number) {
    playerStore.update(
      updateEntities(playerId, { hasOwnParty: true, isInGroup: true }),
    );
  }

  mockPlayerLeaveParty(playerId: number) {
    const player = playerStore.query(getEntity(playerId));

    playerStore.update(
      updateEntities(playerId, {
        hasOwnParty: false,
        invitedLocalPlayer: false,
        isInGroup: false,
      }),
    );
    if (this.connected) {
      if (player.isInGroup) {
        this.emit(
          'partyInfo',
          playerStore
            .query(getAllEntities())
            .filter(p => p.isInGroup)
            .map(p => p.id),
          this.localPlayerId,
        );
      } else if (player.hasOwnParty) {
        this.emit('partyInfo', [], -1);
      }
    }
  }

  updateMockDebugData() {
    const debugData = [
      Math.floor(Math.random() * 100),
      Math.floor(Math.random() * 100),
      Math.floor(Math.random() * 100),
      Math.floor((Math.random() / 4) * 100),
      Math.random() * 100,
      Math.random() * 100,
    ];
    this.emit('debugData', ...debugData);
    return debugData;
  }
}

export function mockSkyrimTogether() {
  (globalThis as any).skyrimtogether = new SkyrimtogetherMock();
}
