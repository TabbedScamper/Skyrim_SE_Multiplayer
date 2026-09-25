import { Component, EventEmitter, HostBinding, HostListener, OnDestroy, OnInit, Output } from '@angular/core';
import { Subject, takeUntil } from 'rxjs';
import { CoopLobbyState } from '../../models/coop-lobby-state';
import { SteamLobbyState } from '../../models/steam-lobby-state';
import { ClientService } from '../../services/client.service';
import { Sound, SoundService } from '../../services/sound.service';
import { SettingService } from '../../services/setting.service';

@Component({
  selector: 'app-title-coop-lobby',
  templateUrl: './title-coop-lobby.component.html',
  styleUrls: ['./title-coop-lobby.component.scss'],
})
export class TitleCoopLobbyComponent implements OnInit, OnDestroy {
  @Output() public done = new EventEmitter<void>();

  public steam: SteamLobbyState = { lobbyId: '', ownerId: '', memberIds: [], memberNames: [], friendIds: [], friendNames: [], open: false, passwordProtected: false, waitingForPassword: false, isHost: false };
  public lobby: CoopLobbyState = { playerIds: [], leaderId: 0, readyPlayerIds: [], campaignMode: 1, sessionState: 0, startEpoch: '0', checkpointId: '', lobbyOpen: false, passwordProtected: false };
  public connected = false;
  public ready = false;
  public campaignMode = 1;
  public checkpointId = '';
  public lanAddress = '';
  public sessionOpen = false;
  public sessionPassword = '';
  public joinPassword = '';
  public scan = { complete: false, fileCount: 0, hashed: 0, cached: 0, errors: 0 };
  public gameplay = { difficulty: 4, pvpEnabled: false, deathSystemEnabled: true, greetingsEnabled: false };

  private readonly destroy$ = new Subject<void>();
  private partyOptionsDirty = false;
  private partyOptionsTimer?: number;
  private gameplayOptionsTimer?: number;

  public constructor(public readonly client: ClientService, private readonly sound: SoundService,
    public readonly settingService: SettingService) {}

  public ngOnInit(): void {
    this.client.connectionStateChange.pipe(takeUntil(this.destroy$)).subscribe(value => this.connected = value);
    this.client.steamLobbyStateChange.pipe(takeUntil(this.destroy$)).subscribe(value => {
      this.steam = value;
      if (!this.partyOptionsDirty) {
        this.sessionOpen = value.open;
      } else if (value.open === this.sessionOpen) {
        // The native lobby has acknowledged the locally edited access mode.
        this.partyOptionsDirty = false;
      }
    });
    this.client.deploymentScanStateChange.pipe(takeUntil(this.destroy$)).subscribe(value => this.scan = value);
    this.client.coopGameplaySettingsChange.pipe(takeUntil(this.destroy$)).subscribe(value => this.gameplay = { ...value });
    this.client.coopLobbyStateChange.pipe(takeUntil(this.destroy$)).subscribe(value => {
      this.lobby = value;
      this.campaignMode = value.campaignMode || this.campaignMode;
      this.checkpointId = value.checkpointId || this.checkpointId;
      this.ready = value.readyPlayerIds.includes(this.client.localPlayerId as unknown as number);
    });
    this.client.refreshSteamLobby();
  }

  public ngOnDestroy(): void {
    if (this.partyOptionsTimer !== undefined) window.clearTimeout(this.partyOptionsTimer);
    if (this.gameplayOptionsTimer !== undefined) window.clearTimeout(this.gameplayOptionsTimer);
    this.destroy$.next();
    this.destroy$.complete();
  }

  /** Controller navigation surface (GamepadNavigationService). Arrow keys move spatially there. */
  @HostBinding('attr.data-nav-scope') readonly navScope = '';

  @HostListener('document:keydown', ['$event'])
  public navigateWithKeyboard(event: KeyboardEvent): void {
    if (event.key === 'Escape' || event.key === 'Esc') {
      this.close();
      event.preventDefault();
      event.stopPropagation();
      return;
    }

    const active = document.activeElement as HTMLInputElement | null;
    if (event.key === 'Enter' || event.key === ' ' || event.key === 'Spacebar') {
      if (active?.matches('.coop-lobby button, .coop-lobby input[type="checkbox"], .coop-lobby summary')) {
        active.click();
        event.preventDefault();
        event.stopPropagation();
      }
      return;
    }

  }

  public host(): void {
    this.sound.play(Sound.Ok);
    this.client.hostSteamSession();
  }

  public joinFriend(steamId: string): void {
    this.sound.play(Sound.Ok);
    this.client.joinSteamFriend(steamId);
  }

  public invite(): void {
    this.sound.play(Sound.Ok);
    this.client.inviteSteamFriend();
  }

  public savePartyOptions(): void {
    if (!this.isLeader()) return;
    this.sound.play(Sound.Ok);
    this.client.setSteamSessionAccess(this.sessionOpen, this.sessionOpen ? this.sessionPassword : '');
  }

  public queuePartyOptionsSave(): void {
    this.partyOptionsDirty = true;
    if (this.partyOptionsTimer !== undefined) window.clearTimeout(this.partyOptionsTimer);
    this.partyOptionsTimer = window.setTimeout(() => {
      this.partyOptionsTimer = undefined;
      this.savePartyOptions();
    }, 200);
  }

  public queueGameplayOptionsSave(): void {
    if (!this.isLeader()) return;
    if (this.gameplayOptionsTimer !== undefined) window.clearTimeout(this.gameplayOptionsTimer);
    this.gameplayOptionsTimer = window.setTimeout(() => {
      this.gameplayOptionsTimer = undefined;
      this.client.setCoopGameplaySettings(this.gameplay.difficulty, this.gameplay.pvpEnabled);
    }, 200);
  }

  public connectWithPassword(): void {
    if (!this.joinPassword) {
      this.sound.play(Sound.Fail);
      return;
    }
    this.client.connectJoinedSteamSession(this.joinPassword);
  }

  public connectLan(): void {
    const match = this.lanAddress.trim().match(/^([^:]+)(?::(\d+))?$/);
    if (!match) {
      this.sound.play(Sound.Fail);
      return;
    }
    this.client.connect(match[1], match[2] ? Number.parseInt(match[2]) : 10578);
  }

  public toggleReady(): void {
    this.ready = !this.ready;
    this.client.setPartyReady(this.ready);
  }

  public chooseCampaign(mode: number): void {
    if (!this.isLeader()) return;
    this.campaignMode = mode;
    this.client.selectSharedCampaign(mode, this.checkpointId.trim());
  }

  public promote(playerId: number): void {
    if (!this.isLeader() || playerId === this.lobby.leaderId) return;
    this.sound.play(Sound.Ok);
    this.client.changePartyLeader(playerId);
  }

  public memberPlayerId(index: number): number {
    return this.lobby.playerIds[index] || 0;
  }

  public campaignLabel(): string {
    return this.campaignMode === 2 ? 'Continue Shared Campaign' : 'New Shared Campaign';
  }

  public leaderName(): string {
    const index = this.lobby.playerIds.indexOf(this.lobby.leaderId);
    return index >= 0 ? (this.steam.memberNames[index] || 'The host') : 'The host';
  }

  public start(): void {
    this.client.startTogether(this.campaignMode, this.checkpointId.trim());
  }

  public isLeader(): boolean {
    return this.client.localPlayerId === this.lobby.leaderId;
  }

  public canStart(): boolean {
    return this.isLeader() && this.lobby.playerIds.length >= 2 &&
      this.lobby.readyPlayerIds.length === this.lobby.playerIds.length && this.campaignMode > 0 && this.lobby.sessionState === 0;
  }

  public isMemberReady(index: number): boolean {
    const id = this.memberPlayerId(index);
    return !!id && this.lobby.readyPlayerIds.includes(id);
  }

  public settingsLocked(): boolean {
    return !this.isLeader() || this.lobby.sessionState === 1 || this.lobby.sessionState === 2;
  }

  /** Why the host cannot start yet, in the order the host should fix it. */
  public startBlocker(): string {
    if (!this.connected) return 'Checking that everyone has the same mod setup...';
    if (!this.scan.complete) return 'Verifying game files...';
    if (this.lobby.playerIds.length < 2) return 'Invite a friend to start.';
    const waiting = this.lobby.playerIds.length - this.lobby.readyPlayerIds.length;
    if (waiting > 0) return waiting === 1 ? 'Waiting for 1 player to ready up.' : `Waiting for ${waiting} players to ready up.`;
    return '';
  }

  public close(): void {
    this.done.emit();
  }
}
