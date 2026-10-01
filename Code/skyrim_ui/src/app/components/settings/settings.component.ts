import { Component, ElementRef, EventEmitter, HostBinding, HostListener, OnDestroy, Output } from '@angular/core';
import { TranslocoService } from '@ngneat/transloco';
import { map, Observable, Subscription } from 'rxjs';
import {
  autoHideTimerLengths,
  FontSize,
  PartyAnchor,
  SettingService,
} from 'src/app/services/setting.service';
import { Sound, SoundService } from '../../services/sound.service';
import { HttpClient } from '@angular/common/http';
import { ClientService } from 'src/app/services/client.service';
import { AudioDevice, DisplayMode, GameSettings } from 'src/app/models/game-settings';

interface UpdateState {
  current: string;
  latest: string;
  phase: string;
  notes: string;
  publishedAt: string;
  received: number;
  size: number;
  error: string;
  lastChecked: number;
}

type SettingsSection = 'display' | 'audio' | 'controls' | 'accessibility' | 'interface' | 'party' | 'updates' | 'about';

@Component({
  selector: 'app-settings',
  templateUrl: './settings.component.html',
  styleUrls: ['./settings.component.scss'],
})
export class SettingsComponent implements OnDestroy {
  readonly DisplayMode = DisplayMode;
  readonly displayModes = [
    { value: DisplayMode.Windowed, label: 'Windowed' },
    { value: DisplayMode.Borderless, label: 'Borderless fullscreen' },
    { value: DisplayMode.Fullscreen, label: 'Exclusive fullscreen' },
  ];
  readonly availableLanguages = this.translocoService.getAvailableLangs();
  readonly availableFontSizes: { id: FontSize; label: string }[] = [
    { id: FontSize.XS, label: 'COMPONENT.SETTINGS.FONT_SIZES.XS' },
    { id: FontSize.S, label: 'COMPONENT.SETTINGS.FONT_SIZES.S' },
    { id: FontSize.M, label: 'COMPONENT.SETTINGS.FONT_SIZES.M' },
    { id: FontSize.L, label: 'COMPONENT.SETTINGS.FONT_SIZES.L' },
    { id: FontSize.XL, label: 'COMPONENT.SETTINGS.FONT_SIZES.XL' },
  ];
  readonly availablePartyAnchors: { id: PartyAnchor; label: string }[] = [
    {
      id: PartyAnchor.TOP_LEFT,
      label: 'COMPONENT.SETTINGS.PARTY_ANCHOR_POSITION.TOP_LEFT',
    },
    {
      id: PartyAnchor.TOP_RIGHT,
      label: 'COMPONENT.SETTINGS.PARTY_ANCHOR_POSITION.TOP_RIGHT',
    },
    {
      id: PartyAnchor.BOTTOM_RIGHT,
      label: 'COMPONENT.SETTINGS.PARTY_ANCHOR_POSITION.BOTTOM_LEFT',
    },
    {
      id: PartyAnchor.BOTTOM_LEFT,
      label: 'COMPONENT.SETTINGS.PARTY_ANCHOR_POSITION.BOTTOM_RIGHT',
    },
  ];
  readonly availableAutoHideTimes = autoHideTimerLengths;
  readonly sections: { id: SettingsSection; label: string }[] = [
    { id: 'display', label: 'Display' },
    { id: 'audio', label: 'Audio' },
    { id: 'controls', label: 'Controls' },
    { id: 'accessibility', label: 'Accessibility' },
    { id: 'interface', label: 'Interface' },
    { id: 'party', label: 'Party HUD' },
    { id: 'updates', label: 'Updates' },
    { id: 'about', label: 'About' },
  ];
  readonly volumeChannels = [
    { key: 'master', label: 'Master volume', hint: 'Hold to hear a mix of game sounds.' },
    { key: 'effects', label: 'Effects', hint: 'Hold to hear combat sounds alone.' },
    { key: 'voice', label: 'Voice', hint: 'Hold to hear a spoken line alone.' },
    { key: 'music', label: 'Music', hint: 'Hold to hear the current music alone.' },
    { key: 'footsteps', label: 'Footsteps', hint: 'Hold to hear footsteps alone.' },
  ];
  public activeSection: SettingsSection = SettingsComponent.restoreSection();
  /** Undefined until the client reports devices (older clients never do). */
  public audioDevices?: AudioDevice[];

  public settings = this.settingService.settings;
  public autoHideTime: number;
  public partyAnchor: PartyAnchor;
  public partyAnchorOffsetX: number;
  public partyAnchorOffsetY: number;
  public fontSize: FontSize;
  public maxFontSize = Object.values(FontSize).length - 1;
  public minFontSize = 0;
  public gameSettings?: GameSettings;
  public monitors: string[] = [];
  public resolutions: string[] = [];
  public displayPreviewSeconds = 0;
  private subscriptions: Subscription[] = [];
  private previewTimer?: ReturnType<typeof setInterval>;

  clientVersion$: Observable<string>;
  /** The updater's report (Options > Updates); undefined until the client answers. */
  public update?: UpdateState;

  @Output() public done = new EventEmitter<void>();
  @Output() public settingsUpdated = new EventEmitter<void>();

  constructor(
    private readonly settingService: SettingService,
    private readonly sound: SoundService,
    private readonly translocoService: TranslocoService,
    private readonly http: HttpClient,
    private readonly client: ClientService,
    private readonly elementRef: ElementRef<HTMLElement>,
  ) {
    this.clientVersion$ = this.client.versionSet.pipe(map(version => version.split('-')[0]));
  }

  ngOnInit(): void {
    this.subscriptions.push(
      this.client.updateStateChange.subscribe(state => {
        if (!state) return;
        try {
          this.update = JSON.parse(state) as UpdateState;
        } catch {
          // A malformed report keeps the last good one.
        }
      }),
      this.client.gameSettingsChange.subscribe(payload => {
        this.gameSettings = { ...payload.settings };
        this.monitors = payload.monitors;
        this.resolutions = payload.resolutions;
        this.audioDevices = payload.audioDevices;
      }),
      this.client.displayPreviewStarted.subscribe(() => this.startDisplayCountdown()),
      this.client.displayPreviewReverted.subscribe(() => {
        this.displayPreviewSeconds = 0;
        this.client.requestGameSettings();
      }),
      this.client.gameSettingsApplied.subscribe(() => {
        this.displayPreviewSeconds = 0;
        if (this.previewTimer) clearInterval(this.previewTimer);
        this.previewTimer = undefined;
      }),
    );
    this.client.requestGameSettings();
    // The title screen may not run the client's update tick; ask once so the panel has a state.
    if (!this.client.updateStateChange.value) this.client.checkUpdates();
  }

  /** Controller navigation surface (GamepadNavigationService); movement is spatial there. */
  @HostBinding('attr.data-nav-scope') readonly navScope = '';

  /**
   * A controller step (or A) on a volume slider: play that channel for a moment.
   * The native preview stops by itself ~1.5 s after the last keep-alive.
   */
  nudgeAudioPreview(channel: string): void {
    if (this.previewKeepAlive) return;
    this.client.audioPreviewKeepAlive(channel);
  }

  /** Keeps the live audio preview going while a volume slider is held. */
  startAudioPreview(channel: string): void {
    this.stopAudioPreview(false);
    this.client.audioPreviewKeepAlive(channel);
    this.previewKeepAlive = setInterval(() => this.client.audioPreviewKeepAlive(channel), 500);
  }

  stopAudioPreview(notify = true): void {
    if (this.previewKeepAlive) clearInterval(this.previewKeepAlive);
    this.previewKeepAlive = undefined;
    if (notify) this.client.audioPreviewStop();
  }

  private previewKeepAlive?: ReturnType<typeof setInterval>;

  /** A release outside the slider still ends the preview. */
  @HostListener('window:pointerup')
  onWindowPointerUp(): void {
    if (this.previewKeepAlive) this.stopAudioPreview();
  }

  ngOnDestroy(): void {
    this.stopAudioPreview();
    this.subscriptions.forEach(subscription => subscription.unsubscribe());
    if (this.previewTimer) clearInterval(this.previewTimer);
    if (this.restoredTimer) clearTimeout(this.restoredTimer);
  }

  close() {
    this.done.next();
    this.sound.play(Sound.Ok);
  }

  selectSection(section: SettingsSection): void {
    this.activeSection = section;
    try {
      localStorage.setItem(SettingsComponent.sectionKey, section);
    } catch {
      // Storage can be unavailable; the section just is not remembered.
    }
    this.sound.play(Sound.Focus);
  }

  isGameSection(section: SettingsSection): boolean {
    return section === 'display' || section === 'audio' || section === 'controls' || section === 'accessibility';
  }

  audioDeviceConnected(id: string): boolean {
    return !!this.audioDevices?.some(device => device.id === id);
  }

  private static readonly sectionKey = 'settings.section';

  private static restoreSection(): SettingsSection {
    try {
      const saved = localStorage.getItem(SettingsComponent.sectionKey) as SettingsSection | null;
      if (saved && ['display', 'audio', 'controls', 'accessibility', 'interface', 'party', 'updates', 'about'].includes(saved))
        return saved;
    } catch {
      // Fall through to the default section.
    }
    return 'display';
  }

  preview(name: keyof GameSettings, value: number | boolean | string): void {
    if (!this.gameSettings) return;
    (this.gameSettings as any)[name] = value;
    this.client.previewGameSetting(name, value);
  }

  get selectedResolution(): string {
    return this.gameSettings ? `${this.gameSettings.width}x${this.gameSettings.height}` : '';
  }

  set selectedResolution(value: string) {
    if (!this.gameSettings || !/^\d+x\d+$/.test(value)) return;
    const [width, height] = value.split('x').map(Number);
    this.gameSettings.width = width;
    this.gameSettings.height = height;
    this.client.previewGameSetting('resolution', value);
  }

  keepDisplaySettings(): void {
    this.client.confirmDisplaySettings();
    this.sound.play(Sound.Ok);
  }

  revertDisplaySettings(): void {
    this.client.revertGameSettings();
    this.sound.play(Sound.Cancel);
  }

  /** What the section's restore button resets, shown in its confirmation. Absent: the section has nothing to restore. */
  private static readonly restoreScope: Partial<Record<SettingsSection, string>> = {
    display: 'Vertical sync and brightness. Your display mode, monitor and resolution stay as they are.',
    audio: 'Every volume, the output device (back to the Windows default) and the interface sounds.',
    controls: 'Mouse and controller sensitivity, invert Y, always run, vibration, and every keyboard, mouse and controller binding (Skyrim\'s default controls).',
    accessibility: 'Dialogue and general subtitles.',
    party: 'Every party HUD option.',
  };

  get restoreDescription(): string | undefined {
    return SettingsComponent.restoreScope[this.activeSection];
  }

  get sectionLabel(): string {
    return this.sections.find(section => section.id === this.activeSection)?.label ?? '';
  }

  /** Section awaiting confirmation in the restore dialog. */
  public confirmRestore?: SettingsSection;
  /** Shown in the footer after a restore, in place of the autosave note. */
  public restoredMessage = '';
  private restoredTimer?: ReturnType<typeof setTimeout>;

  resetDefaults(): void {
    if (!this.restoreDescription) return;
    this.confirmRestore = this.activeSection;
    this.sound.play(Sound.Focus);
  }

  cancelRestore(): void {
    this.confirmRestore = undefined;
    this.sound.play(Sound.Cancel);
  }

  confirmRestoreDefaults(): void {
    const section = this.confirmRestore;
    this.confirmRestore = undefined;
    if (!section) return;
    const ui = this.settingService.settings;
    switch (section) {
      case 'display':
      case 'accessibility':
      case 'controls':
        this.client.resetGameSettings(section);
        break;
      case 'audio':
        this.client.resetGameSettings(section);
        ui.muted.reset();
        ui.volume.reset();
        break;
      case 'party':
        ui.isPartyShown.reset();
        ui.autoHideParty.reset();
        ui.autoHideTime.reset();
        ui.partyAnchor.reset();
        ui.partyAnchorOffsetX.reset();
        ui.partyAnchorOffsetY.reset();
        break;
    }
    this.sound.play(Sound.Ok);
    this.restoredMessage = `${this.sections.find(s => s.id === section)?.label ?? 'Section'} restored to defaults.`;
    if (this.restoredTimer) clearTimeout(this.restoredTimer);
    this.restoredTimer = setTimeout(() => (this.restoredMessage = ''), 4000);
  }

  private startDisplayCountdown(): void {
    this.displayPreviewSeconds = 15;
    if (this.previewTimer) clearInterval(this.previewTimer);
    this.previewTimer = setInterval(() => {
      this.displayPreviewSeconds = Math.max(0, this.displayPreviewSeconds - 1);
      if (!this.displayPreviewSeconds && this.previewTimer) {
        clearInterval(this.previewTimer);
        this.previewTimer = undefined;
      }
    }, 1000);
  }

  checkUpdates(): void {
    this.client.checkUpdates();
  }

  downloadUpdate(): void {
    this.client.downloadUpdate();
  }

  get updateAvailable(): boolean {
    return this.update?.phase === 'available';
  }

  get updateBusy(): boolean {
    const phase = this.update?.phase;
    return phase === 'checking' || phase === 'downloading' || phase === 'verifying';
  }

  /** One line for where the updater stands. */
  get updateStatus(): string {
    const u = this.update;
    if (!u) return 'Checking for updates...';
    switch (u.phase) {
      case 'idle':
      case 'checking':
        return 'Checking for updates...';
      case 'current':
        return 'You have the latest version.';
      case 'available':
        return `Version ${u.latest} is available.`;
      case 'downloading':
        return u.size ? `Downloading... ${Math.min(100, Math.floor((100 * u.received) / u.size))}%` : 'Downloading...';
      case 'verifying':
        return 'Checking the download...';
      case 'ready':
        return `Version ${u.latest} is ready. Restart the game to finish updating.`;
      case 'offline':
        return 'Could not reach GitHub. You can keep playing; it will try again later.';
      case 'error':
        return `The update did not finish: ${u.error}`;
      default:
        return '';
    }
  }

  /** Patch notes as plain lines (markdown markers removed; never rendered as HTML). */
  get patchNotes(): { text: string; heading: boolean; bullet: boolean }[] {
    return (this.update?.notes ?? '')
      .split('\n')
      .map(line => line.replace(/\s+$/, ''))
      .filter(line => line.trim().length)
      .slice(0, 200)
      .map(line => {
        const heading = /^#{1,6}\s/.test(line);
        const bullet = /^\s*[-*]\s/.test(line);
        const text = line
          .replace(/^#{1,6}\s+/, '')
          .replace(/^\s*[-*]\s+/, '')
          .replace(/\*\*(.+?)\*\*/g, '$1')
          .replace(/`([^`]+)`/g, '$1')
          .replace(/\[([^\]]+)\]\([^)]+\)/g, '$1');
        return { text, heading, bullet };
      });
  }

  @HostListener('window:keydown.escape', ['$event'])
  // @ts-ignore
  private activate(event: KeyboardEvent): void {
    // Only the Settings window on screen answers (another instance can exist hidden).
    if (!this.elementRef.nativeElement.getClientRects().length) return;
    if (this.confirmRestore) this.cancelRestore();
    else if (this.displayPreviewSeconds) this.revertDisplaySettings();
    else this.close();
    event.stopPropagation();
    event.preventDefault();
  }
}
