import { Component, EventEmitter, OnDestroy, OnInit, Output } from '@angular/core';
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

  public constructor(public readonly client: ClientService, private readonly sound: SoundService) {}

  public ngOnInit(): void {
    this.client.connectionStateChange.pipe(takeUntil(this.destroy$)).subscribe(value => this.connected = value);
    this.client.steamLobbyStateChange.pipe(takeUntil(this.destroy$)).subscribe(value => {
      this.steam = value;
      this.sessionOpen = value.open;
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
    this.destroy$.next();
    this.destroy$.complete();
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
