import { Component, EventEmitter, HostBinding, HostListener, Input, OnDestroy, OnInit, Output } from '@angular/core';
import { Subject, takeUntil } from 'rxjs';
import { CheckpointEntry, CoopLobbyState, JoinCharacter } from '../../models/coop-lobby-state';
import { SteamFriend, SteamInvite, SteamLobbyState } from '../../models/steam-lobby-state';
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
  @Output() public settingsRequested = new EventEmitter<void>();
  @Input() public inGame = false;
  public confirmingLeave = false;

  public steam: SteamLobbyState = { lobbyId: '', ownerId: '', memberIds: [], memberNames: [], friendIds: [], friendNames: [], open: false, passwordProtected: false, waitingForPassword: false, isHost: false, friends: [], offlineFriends: 0, invites: [] };
  public avatars: Record<string, string> = {};
  public lobby: CoopLobbyState = { playerIds: [], leaderId: 0, readyPlayerIds: [], campaignMode: 1, sessionState: 0, startEpoch: '0', checkpointId: '', lobbyOpen: false, passwordProtected: false };
  public connected = false;
  public ready = false;
  public campaignMode = 1;
  public checkpointId = '';
  public checkpoints: CheckpointEntry[] = [];
  public characters: JoinCharacter[] = [];
  public dropInStatus = 'idle';
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
    this.client.steamAvatars.pipe(takeUntil(this.destroy$)).subscribe(value => this.avatars = value);
    this.client.coopGameplaySettingsChange.pipe(takeUntil(this.destroy$)).subscribe(value => this.gameplay = { ...value });
    this.client.coopLobbyStateChange.pipe(takeUntil(this.destroy$)).subscribe(value => {
      this.lobby = value;
      this.campaignMode = value.campaignMode || this.campaignMode;
      this.checkpointId = value.checkpointId || this.checkpointId;
      this.ready = value.readyPlayerIds.includes(this.client.localPlayerId as unknown as number);
    });
    this.client.checkpointListChange.pipe(takeUntil(this.destroy$)).subscribe(value => this.checkpoints = value);
    this.client.characterListChange.pipe(takeUntil(this.destroy$)).subscribe(value => this.characters = value);
    this.client.dropInStatusChange.pipe(takeUntil(this.destroy$)).subscribe(value => this.dropInStatus = value);
    this.client.listCharacters();
    this.client.refreshSteamLobby();
    this.client.listCheckpoints();
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
    if (this.sessionStarted) return;
    this.sound.play(Sound.Ok);
    this.client.hostSteamSession();
  }

  public joinFriend(steamId: string): void {
    if (this.sessionStarted) return;
    this.sound.play(Sound.Ok);
    this.client.joinSteamFriend(steamId);
  }

  public invite(): void {
    this.sound.play(Sound.Ok);
    this.client.inviteSteamFriend();
  }

  /** Friends in the order you would reach for them: in a session, playing Skyrim, online, away. */
  get friends(): SteamFriend[] {
    const rank: Record<SteamFriend['status'], number> = { coop: 0, skyrim: 1, online: 2, busy: 3, away: 4 };
    return [...(this.steam.friends ?? [])]
      .filter(friend => !friend.inLobby)
      .sort((a, b) => rank[a.status] - rank[b.status] || a.name.localeCompare(b.name));
  }

  friendStatus(friend: SteamFriend): string {
    switch (friend.status) {
      case 'coop': return 'In a co-op session';
      case 'skyrim': return 'Playing Skyrim';
      case 'busy': return 'Busy';
      case 'away': return 'Away';
      default: return 'Online';
    }
  }

  inviteFriend(friend: SteamFriend): void {
    this.sound.play(Sound.Ok);
    this.client.inviteSteamFriend(friend.id);
  }

  answerInvite(invite: SteamInvite, accept: boolean): void {
    if (accept && this.sessionStarted) return;
    this.sound.play(accept ? Sound.Ok : Sound.Cancel);
    this.client.answerSteamInvite(invite.lobby, accept);
  }

  initial(name: string): string {
    return (name || '?').trim().charAt(0).toUpperCase();
  }

  trackFriend(_: number, friend: SteamFriend): string {
    return friend.id;
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
    if (this.settingsLocked()) return;
    if (this.gameplayOptionsTimer !== undefined) window.clearTimeout(this.gameplayOptionsTimer);
    this.gameplayOptionsTimer = window.setTimeout(() => {
      this.gameplayOptionsTimer = undefined;
      // Recheck after debounce: loading/creation or leadership can change.
      if (this.settingsLocked()) return;
      this.client.setCoopGameplaySettings(this.gameplay.difficulty, this.gameplay.pvpEnabled);
    }, 200);
  }

  public connectWithPassword(): void {
    if (this.sessionStarted) return;
    if (!this.joinPassword) {
      this.sound.play(Sound.Fail);
      return;
    }
    this.client.connectJoinedSteamSession(this.joinPassword);
  }

  public connectLan(): void {
    if (this.sessionStarted) return;
    const match = this.lanAddress.trim().match(/^([^:]+)(?::(\d+))?$/);
    if (!match) {
      this.sound.play(Sound.Fail);
      return;
    }
    this.client.connect(match[1], match[2] ? Number.parseInt(match[2]) : 10578);
  }

  public toggleReady(): void {
    if (this.sessionStarted || !this.connected) return;
    this.ready = !this.ready;
    this.client.setPartyReady(this.ready);
  }

  public chooseCampaign(mode: number): void {
    if (this.sessionStarted || !this.isLeader()) return;
    this.campaignMode = mode;
    this.client.selectSharedCampaign(mode, this.checkpointId.trim());
    if (mode === 2) this.client.listCheckpoints();
  }

  /** The session is already running and this PC is not in it yet: offer to join with an own character. */
  public canDropIn(): boolean {
    return !this.inGame && this.connected && this.lobby.sessionState === 3 && !this.isLeader();
  }

  public joining(): boolean {
    return this.dropInStatus !== 'idle' && !this.dropInStatus.startsWith('failed') && this.dropInStatus !== 'joined';
  }

  public joinWith(character: JoinCharacter): void {
    if (this.joining()) return;
    this.client.joinRunningSession(character.path);
  }

  public dropInLabel(): string {
    const status = this.dropInStatus;
    if (status.startsWith('failed: ')) return 'Could not join: ' + status.substring(8);
    return status.charAt(0).toUpperCase() + status.slice(1) + (status === 'joined' ? '' : '...');
  }

  public chooseCheckpoint(id: string): void {
    if (this.sessionStarted || !this.isLeader()) return;
    this.checkpointId = id;
    this.client.selectSharedCampaign(2, id);
  }

  /** An empty choice means the newest checkpoint, which is first in the list. */
  public isChosen(entry: CheckpointEntry, index: number): boolean {
    return this.checkpointId ? entry.id === this.checkpointId : index === 0;
  }

  /** The save's play time, stored as days.hours.minutes ("000.05.09"). */
  public playTime(value: string): string {
    const [days, hours, minutes] = (value || '').split('.').map(part => parseInt(part, 10) || 0);
    return days ? `${days}d ${hours}h ${minutes}m` : `${hours}h ${minutes}m`;
  }

  public chosenEntry(): CheckpointEntry | undefined {
    const id = this.lobby.checkpointId || this.checkpointId;
    return id ? this.checkpoints.find(entry => entry.id === id) : this.checkpoints[0];
  }

  public promote(playerId: number): void {
    if (this.sessionStarted || !this.isLeader() || playerId === this.lobby.leaderId) return;
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
    if (!this.canStart()) return;
    this.client.startTogether(this.campaignMode, this.checkpointId.trim());
  }

  public isLeader(): boolean {
    return this.client.localPlayerId === this.lobby.leaderId;
  }

  public canStart(): boolean {
    return !this.sessionStarted && this.connected && this.scan.complete && this.isLeader() && this.lobby.playerIds.length >= 2 &&
      this.lobby.readyPlayerIds.length === this.lobby.playerIds.length && this.campaignMode > 0 && this.lobby.sessionState === 0;
  }

  public isMemberReady(index: number): boolean {
    const id = this.memberPlayerId(index);
    return !!id && this.lobby.readyPlayerIds.includes(id);
  }

  public settingsLocked(): boolean {
    // Server PartyService::OnPartyGameplaySettings accepts the host's live
    // difficulty/PvP changes in gameplay (3), but not loading/creation (1/2).
    return !this.connected || !this.isLeader() ||
      (this.lobby.sessionState !== 0 && this.lobby.sessionState !== 3) ||
      (this.inGame && this.lobby.sessionState !== 3);
  }

  public get sessionStarted(): boolean {
    return this.inGame || this.lobby.sessionState !== 0;
  }

  public leave(): void {
    if (!this.confirmingLeave) {
      this.confirmingLeave = true;
      return;
    }
    // OnDisconnected owns leaving Steam and stopping the host's server.
    // Clearing the lobby first would make that cleanup return early.
    if (this.connected) this.client.disconnect();
    else this.client.leaveSteamSession();
    this.close();
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
