import { Overlay } from '@angular/cdk/overlay';
import { Component, OnInit, ViewChild } from '@angular/core';
import { TranslocoService } from '@ngneat/transloco';
import { takeUntil } from 'rxjs';
import { environment } from '../../../environments/environment';
import { fadeInOutActiveAnimation } from '../../animations/fade-in-out-active.animation';
import { View } from '../../models/view.enum';
import { ClientService } from '../../services/client.service';
import { DestroyService } from '../../services/destroy.service';
import {
  SettingService,
  fontSizeToPixels,
} from '../../services/setting.service';
import { Sound, SoundService } from '../../services/sound.service';
import { UiRepository } from '../../store/ui.repository';
import { ChatComponent } from '../chat/chat.component';
import { GroupComponent } from '../group/group.component';
import { controlsAnimation } from './controls.animation';
import { notificationsAnimation } from './notifications.animation';
import { map } from 'rxjs/operators';

const REVEAL_EFFECT_DURATION_MS = 10000 // todo: pass value from C++?

@Component({
  selector: 'app-root',
  templateUrl: './root.component.html',
  styleUrls: ['./root.component.scss'],
  animations: [
    controlsAnimation,
    fadeInOutActiveAnimation,
    notificationsAnimation,
  ],
  host: { 'data-app-root-game': environment.game.toString() },
  providers: [DestroyService],
})
export class RootComponent implements OnInit {
  /* ### ENUMS ### */
  readonly RootView = View;

  view$ = this.uiRepository.view$;

  connected$ = this.client.connectionStateChange.asObservable();
  menuOpen$ = this.client.openingMenuChange.asObservable();
  inGame$ = this.client.inGameStateChange.asObservable();
  titleScreen$ = this.client.titleScreenStateChange.asObservable();
  active$ = this.client.activationStateChange.asObservable();
  connectionInProgress$ = this.client.isConnectionInProgressChange.asObservable();
  revealingInProgress$ = false;
  public debugPromptText: string | null = null;
  public debugFeedbackNote = '';
  public debugFeedbackNeedsNote = false;
  public debugFeedbackTitle = 'Visual check';

  @ViewChild('chat') private chatComp!: ChatComponent;
  @ViewChild(GroupComponent) private groupComponent: GroupComponent;

  public constructor(
    private readonly destroy$: DestroyService,
    private readonly client: ClientService,
    private readonly sound: SoundService,
    private readonly uiRepository: UiRepository,
    private readonly translocoService: TranslocoService,
    private readonly settingService: SettingService,
    public readonly overlay: Overlay, // used for mockup
  ) {
    this.translocoService.setActiveLang(
      this.settingService.settings.language.getValue(),
    );
  }

  public ngOnInit(): void {
    this.onInGameStateSubscription();
    this.onActivationStateSubscription();
    this.onFontSizeSubscription();
    this.client.titleOptionsRequested
      .pipe(takeUntil(this.destroy$))
      .subscribe(() => this.setView(View.SETTINGS));
    this.client.debugPrompt
      .pipe(takeUntil(this.destroy$))
      .subscribe(payload => {
        const separator = payload.indexOf('\n');
        const mode = separator >= 0 ? payload.substring(0, separator) : 'check';
        this.debugPromptText = separator >= 0 ? payload.substring(separator + 1) : payload;
        this.debugFeedbackTitle = mode === 'report' ? 'Report a problem' : 'Visual check';
        this.debugFeedbackNote = '';
        this.debugFeedbackNeedsNote = mode === 'report';
      });
    this.client.debugPromptCancelled
      .pipe(takeUntil(this.destroy$))
      .subscribe(() => this.cancelDebugPrompt());
    this.client.debugPromptSubmitted
      .pipe(takeUntil(this.destroy$))
      .subscribe(() => {
        if (this.debugPromptText && this.debugFeedbackNeedsNote) {
          this.submitDebugProblem();
        }
      });
  }

  public onInGameStateSubscription() {
    this.client.inGameStateChange
      .pipe(takeUntil(this.destroy$))
      .subscribe(state => {
        if (!state) {
          this.closeView();
        }
      });
  }

  public onActivationStateSubscription() {
    this.client.activationStateChange
      .pipe(takeUntil(this.destroy$))
      .subscribe(state => {
        if (
          this.client.inGameStateChange.getValue() &&
          state &&
          !this.uiRepository.isViewOpen()
        ) {
          setTimeout(() => this.chatComp.focus(), 100);
        }
        if (!state) {
          this.closeView();
        }
      });
  }

  public onFontSizeSubscription() {
    this.settingService.settings.fontSize
      .pipe(
        takeUntil(this.destroy$),
        map(size => fontSizeToPixels[size]),
      )
      .subscribe(size => {
        document.documentElement.setAttribute('style', `font-size: ${size}px;`);
      });
  }

  public setView(view: View | null) {
    this.uiRepository.openView(view);

    if (view) {
      this.sound.play(Sound.Focus);
    } else if (this.chatComp) {
      this.chatComp.focus();
    }
  }

  public closeView() {
    this.uiRepository.openView(null);
    if (this.client.titleScreenStateChange.getValue()) {
      this.client.deactivate();
    }
  }

  public openTitleOptions(): void {
    this.client.openTitleOptions();
    this.setView(View.SETTINGS);
  }

  public answerDebugPrompt(looksRight: boolean): void {
    if (looksRight) {
      this.client.submitDebugFeedback(true);
      this.debugPromptText = null;
      this.debugFeedbackNeedsNote = false;
      this.client.deactivate();
      return;
    }
    this.debugFeedbackNeedsNote = true;
  }

  public submitDebugProblem(): void {
    this.client.submitDebugFeedback(false, this.debugFeedbackNote.trim());
    this.debugPromptText = null;
    this.debugFeedbackNeedsNote = false;
    this.client.deactivate();
  }

  public cancelDebugPrompt(): void {
    this.debugPromptText = null;
    this.debugFeedbackNote = '';
    this.debugFeedbackNeedsNote = false;
  }

  public reconnect(): void {
    this.client.reconnect();
  }

  public revealPlayers(): void {
    if (this.revealingInProgress$)
      return;

    this.revealingInProgress$ = true;
    setTimeout(() => { this.revealingInProgress$ = false }, REVEAL_EFFECT_DURATION_MS);

    this.sound.play(Sound.Focus);
    this.client.revealPlayers();
  }
}
