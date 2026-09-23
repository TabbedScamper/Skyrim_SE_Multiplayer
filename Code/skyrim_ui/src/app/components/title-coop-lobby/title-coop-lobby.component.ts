import { Component, EventEmitter, HostListener, OnDestroy, OnInit, Output } from '@angular/core';
import { Subject, takeUntil } from 'rxjs';
import { CoopLobbyState } from '../../models/coop-lobby-state';
import { SteamLobbyState } from '../../models/steam-lobby-state';
import { ClientService } from '../../services/client.service';
import { Sound, SoundService } from '../../services/sound.service';

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

  private readonly destroy$ = new Subject<void>();
  private partyOptionsDirty = false;
  private partyOptionsTimer?: number;

  public constructor(public readonly client: ClientService, private readonly sound: SoundService) {}

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
    this.destroy$.next();
    this.destroy$.complete();
  }

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

    if (!['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight'].includes(event.key)) return;

    if (active?.tagName === 'INPUT' && !['button', 'checkbox', 'radio'].includes(active.type)) return;

    const controls = Array.from(document.querySelectorAll<HTMLElement>(
      '.coop-lobby button:not(:disabled), .coop-lobby input:not(:disabled), .coop-lobby summary'))
      .filter(control => control.offsetParent !== null);
    if (controls.length === 0) return;

    const current = controls.indexOf(document.activeElement as HTMLElement);
    const direction = event.key === 'ArrowDown' || event.key === 'ArrowRight' ? 1 : -1;
    const next = current < 0
      ? (direction > 0 ? 0 : controls.length - 1)
      : (current + direction + controls.length) % controls.length;
    controls[next].focus();
    event.preventDefault();
    event.stopPropagation();
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

  public start(): void {
    this.client.startTogether(this.campaignMode, this.checkpointId.trim());
  }

  public isLeader(): boolean {
    return this.client.localPlayerId === this.lobby.leaderId;
  }

  public canStart(): boolean {
    return this.isLeader() && this.lobby.playerIds.length === 2 &&
      this.lobby.readyPlayerIds.length === this.lobby.playerIds.length && this.campaignMode > 0 && this.lobby.sessionState === 0;
  }

  public close(): void {
    this.done.emit();
  }
}
