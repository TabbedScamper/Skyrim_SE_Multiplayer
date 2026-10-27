import { CommonModule } from '@angular/common';
import { Component, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { ClientService } from '../../services/client.service';

/**
 * The live main-menu lobby (LobbyService, client). Its splash (the SKYRIM wordmark over Skyrim's smoke) and its menu
 * are Skyrim's own; this layer only shows the lobby soundtrack's playing track.
 */
@Component({
  selector: 'app-lobby-screen',
  standalone: true,
  imports: [CommonModule],
  templateUrl: './lobby-screen.component.html',
  styleUrls: ['./lobby-screen.component.scss'],
})
export class LobbyScreenComponent implements OnDestroy {
  /** Official title of the playing soundtrack track. */
  music = '';

  private readonly musicSubscription: Subscription;

  constructor(private readonly client: ClientService) {
    this.musicSubscription = this.client.lobbyMusic.subscribe(title => (this.music = title));
  }

  ngOnDestroy(): void {
    this.musicSubscription.unsubscribe();
  }
}
