import { Component, ElementRef, EventEmitter, HostListener, OnDestroy, Output } from '@angular/core';
import { TranslocoService } from '@ngneat/transloco';
import { lastValueFrom, map, Observable, Subscription } from 'rxjs';
import { Tag } from 'src/app/models/tag';
import {
  autoHideTimerLengths,
  FontSize,
  PartyAnchor,
  SettingService,
} from 'src/app/services/setting.service';
import { Sound, SoundService } from '../../services/sound.service';
import { environment } from 'src/environments/environment';
import { HttpClient } from '@angular/common/http';
import { ClientService } from 'src/app/services/client.service';
import { AudioDevice, DisplayMode, GameSettings } from 'src/app/models/game-settings';

type SettingsSection = 'display' | 'audio' | 'controls' | 'accessibility' | 'interface' | 'party' | 'about';

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
    { id: 'about', label: 'About' },
  ];
  readonly volumeChannels = [
    { key: 'master', label: 'Master volume', hint: 'Plays a sample while you adjust it.' },
    { key: 'effects', label: 'Effects', hint: 'Plays a sword strike while you adjust it.' },
    { key: 'voice', label: 'Voice', hint: 'Plays a spoken line while you adjust it.' },
    { key: 'music', label: 'Music', hint: 'Adjusts the music that is playing now.' },
    { key: 'footsteps', label: 'Footsteps', hint: 'Plays footsteps while you adjust it.' },
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
  isVersionOutdated: Promise<boolean>;

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
    this.isVersionOutdated = this.isGameVersionOutdated();
    this.subscriptions.push(
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
  }

  @HostListener('keydown.arrowdown', ['$event'])
  focusNext(event: KeyboardEvent): void {
    this.moveFocus(1, event);
  }

  @HostListener('keydown.arrowup', ['$event'])
  focusPrevious(event: KeyboardEvent): void {
    this.moveFocus(-1, event);
  }

  private moveFocus(direction: number, event: KeyboardEvent): void {
    if (event.defaultPrevented) return;
    const focusable = Array.from(
      this.elementRef.nativeElement.querySelectorAll<HTMLElement>(
        'app-dropdown, input:not([disabled]), button:not([disabled])',
      ),
    ).filter(element => element.offsetParent !== null);
    if (!focusable.length) return;
    const current = focusable.indexOf(document.activeElement as HTMLElement);
    const next = current < 0
      ? (direction > 0 ? 0 : focusable.length - 1)
      : (current + direction + focusable.length) % focusable.length;
    focusable[next].focus();
    focusable[next].scrollIntoView({ behavior: 'smooth', block: 'nearest' });
    event.preventDefault();
    event.stopPropagation();
  }

  ngOnDestroy(): void {
    this.subscriptions.forEach(subscription => subscription.unsubscribe());
    if (this.previewTimer) clearInterval(this.previewTimer);
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
      if (saved && ['display', 'audio', 'controls', 'accessibility', 'interface', 'party', 'about'].includes(saved))
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

  resetDefaults(): void {
    this.client.resetGameSettings();
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

  private getVersionTagList(): Promise<Tag[]> {
    return lastValueFrom(
      this.http
        .get<Tag[]>(`${ environment.githubUrl }`));
  }

  async isGameVersionOutdated(): Promise<boolean> {
    let usedVersion = this.client.getVersion();

    const tags = await this.getVersionTagList();
    const usedVersionIndex = tags.findIndex(tag => tag.name === usedVersion);

    return usedVersionIndex > 0 || usedVersionIndex === -1;
  }

  @HostListener('window:keydown.escape', ['$event'])
  // @ts-ignore
  private activate(event: KeyboardEvent): void {
    if (this.displayPreviewSeconds) this.revertDisplaySettings();
    else this.close();
    event.stopPropagation();
    event.preventDefault();
  }
}
