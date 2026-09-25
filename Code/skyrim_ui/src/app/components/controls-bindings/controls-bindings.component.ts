import { Component, HostListener, OnDestroy, OnInit } from '@angular/core';
import { Subject, takeUntil } from 'rxjs';
import { ControlBinding, ControlBindingsState, InputDevice } from '../../models/control-bindings';
import { ClientService } from '../../services/client.service';
import { Sound, SoundService } from '../../services/sound.service';
import { eventLabel, keyboardName, mouseName, padName } from './key-names';
import { CONTROLLER_MODELS, ControllerModel, ControllerModelId, MODEL_CHOICES, PadButton } from './controller-models';

interface ActionRow {
  event: string;
  label: string;
  keyboard?: number;
  mouse?: number;
  gamepad?: number;
}

/** Actions in the order players look for them; anything else follows alphabetically. */
const ACTION_ORDER = [
  'Forward', 'Back', 'Strafe Left', 'Strafe Right', 'Jump', 'Sprint', 'Sneak', 'Run', 'Toggle Always Run', 'Auto-Move',
  'Activate', 'Ready Weapon', 'Left Attack/Block', 'Right Attack/Block', 'Shout', 'Toggle POV',
  'Tween Menu', 'Favorites', 'Quick Inventory', 'Quick Magic', 'Quick Map', 'Quick Stats', 'Journal', 'Wait',
  'Hotkey1', 'Hotkey2', 'Hotkey3', 'Hotkey4', 'Hotkey5', 'Hotkey6', 'Hotkey7', 'Hotkey8', 'Quicksave', 'Quickload',
];

@Component({
  selector: 'app-controls-bindings',
  templateUrl: './controls-bindings.component.html',
  styleUrls: ['./controls-bindings.component.scss'],
})
export class ControlsBindingsComponent implements OnInit, OnDestroy {
  readonly InputDevice = InputDevice;
  public mode: 'controller' | 'keyboard' = 'controller';
  public state?: ControlBindingsState;
  public actions: ActionRow[] = [];
  public capture?: { event: string; device: InputDevice };
  /** Button hovered or clicked on the wireframe: its actions are highlighted. */
  public focusKey?: number;

  private readonly destroy$ = new Subject<void>();

  constructor(private readonly client: ClientService, private readonly sound: SoundService) {}

  ngOnInit(): void {
    this.client.controlBindingsChange.pipe(takeUntil(this.destroy$)).subscribe(state => {
      this.state = state;
      this.actions = this.buildActions(state.bindings);
      if (this.capture) this.sound.play(Sound.Ok);
      this.capture = undefined;
    });
    this.client.requestControlBindings();
  }

  ngOnDestroy(): void {
    if (this.capture) this.client.cancelControlCapture();
    this.destroy$.next();
    this.destroy$.complete();
  }

  readonly modelChoices = MODEL_CHOICES;
  /** '' = use the detected model. Remembered per PC. */
  public modelOverride: ControllerModelId | '' = ControlsBindingsComponent.restoreOverride();

  get detectedModel(): ControllerModel {
    const id = (this.state?.controller ?? 'xbox-series') as ControllerModelId;
    return CONTROLLER_MODELS[id] ?? CONTROLLER_MODELS['xbox-series'];
  }

  get model(): ControllerModel {
    return this.modelOverride ? CONTROLLER_MODELS[this.modelOverride] : this.detectedModel;
  }

  /**
   * Callout rows are laid out here, not per model: each side keeps the model's
   * top-to-bottom order but gets evenly spaced two-line rows, so labels never
   * overlap and text can stay at the 13px floor.
   */
  get buttons(): PadButton[] {
    const model = this.model;
    if (this.layoutFor === model) return this.laidOut;
    const place = (side: 'left' | 'right') => model.buttons
      .filter(b => b.side === side)
      .sort((a, b) => a.labelY - b.labelY)
      .map((b, row) => ({ ...b, labelY: ControlsBindingsComponent.firstRow + row * ControlsBindingsComponent.rowStep }));
    this.laidOut = [...place('left'), ...place('right')];
    this.layoutFor = model;
    return this.laidOut;
  }

  private static readonly firstRow = 24;
  private static readonly rowStep = 41;
  private layoutFor?: ControllerModel;
  private laidOut: PadButton[] = [];

  setModelOverride(id: ControllerModelId | ''): void {
    this.modelOverride = id;
    this.focusKey = undefined;
    try {
      localStorage.setItem(ControlsBindingsComponent.overrideKey, id);
    } catch {
      // Not remembered when storage is unavailable.
    }
  }

  private static readonly overrideKey = 'controls.model';

  private static restoreOverride(): ControllerModelId | '' {
    try {
      const saved = localStorage.getItem(ControlsBindingsComponent.overrideKey) as ControllerModelId | null;
      return saved && saved in CONTROLLER_MODELS ? saved : '';
    } catch {
      return '';
    }
  }

  /** SVG points for a PlayStation face symbol centred on the button. */
  symbolPath(button: PadButton): string {
    const { x, y } = button;
    const glyph = button.glyph;
    if (!glyph || !('symbol' in glyph)) return '';
    switch (glyph.symbol) {
      case 'triangle': return `M${x},${y - 7} L${x + 7},${y + 5} L${x - 7},${y + 5} Z`;
      case 'square': return `M${x - 6},${y - 6} h12 v12 h-12 Z`;
      case 'cross': return `M${x - 6},${y - 6} L${x + 6},${y + 6} M${x + 6},${y - 6} L${x - 6},${y + 6}`;
      case 'circle': return `M${x - 7},${y} a7,7 0 1,0 14,0 a7,7 0 1,0 -14,0`;
    }
  }

  /** Centre of the D-pad cross (mean of its four arms). */
  get dpadCenter(): { x: number; y: number } | undefined {
    const arms = this.model.buttons.filter(b => b.shape === 'dpad');
    if (arms.length !== 4) return undefined;
    return { x: arms.reduce((t, b) => t + b.x, 0) / 4, y: arms.reduce((t, b) => t + b.y, 0) / 4 };
  }

  /** Solid D-pad cross outline around its centre. */
  dpadPath(c: { x: number; y: number }): string {
    const a = 11, l = 31; // half arm width, arm reach
    return `M${c.x - a},${c.y - l} h${2 * a} v${l - a} h${l - a} v${2 * a} h${-(l - a)} v${l - a} h${-2 * a} ` +
      `v${-(l - a)} h${-(l - a)} v${-2 * a} h${l - a} Z`;
  }

  /** Small arrow on a D-pad arm, pointing away from the centre. */
  dpadArrow(b: PadButton): string {
    const c = this.dpadCenter;
    if (!c) return '';
    const dx = Math.sign(Math.round(b.x - c.x)), dy = Math.sign(Math.round(b.y - c.y));
    const x = b.x + dx * 6, y = b.y + dy * 6, s = 5;
    if (dy < 0) return `M${x},${y - s} L${x + s},${y + s * 0.6} L${x - s},${y + s * 0.6} Z`;
    if (dy > 0) return `M${x},${y + s} L${x + s},${y - s * 0.6} L${x - s},${y - s * 0.6} Z`;
    if (dx < 0) return `M${x - s},${y} L${x + s * 0.6},${y - s} L${x + s * 0.6},${y + s} Z`;
    return `M${x + s},${y} L${x - s * 0.6},${y - s} L${x - s * 0.6},${y + s} Z`;
  }

  /** Trigger: a rounded cap whose top follows the shoulder. */
  triggerPath(b: PadButton): string {
    const w = 30, h = 20;
    return `M${b.x - w},${b.y + h / 2} L${b.x - w},${b.y - h / 4} Q${b.x - w},${b.y - h / 2 - 6} ${b.x},${b.y - h / 2 - 8} ` +
      `Q${b.x + w},${b.y - h / 2 - 6} ${b.x + w},${b.y - h / 4} L${b.x + w},${b.y + h / 2} Z`;
  }

  /** Bumper: a shallow arc band along the top edge. */
  bumperPath(b: PadButton): string {
    const w = 40;
    return `M${b.x - w},${b.y + 5} Q${b.x},${b.y - 11} ${b.x + w},${b.y + 5} L${b.x + w},${b.y - 2} ` +
      `Q${b.x},${b.y - 18} ${b.x - w},${b.y - 2} Z`;
  }

  isUnbound(key: number): boolean {
    return this.actionsOn(key) === '-';
  }

  letterOf(button: PadButton): string {
    return button.glyph && 'letter' in button.glyph ? button.glyph.letter : '';
  }

  /** Elbowed leader line from the button to its label column. */
  leader(button: PadButton): string {
    const labelX = button.side === 'left' ? 44 : 596;
    const elbowX = button.side === 'left' ? 72 : 568;
    return `${button.x},${button.y} ${elbowX},${button.labelY} ${labelX},${button.labelY}`;
  }

  padLabel(key: number): string {
    return padName(key, this.model.labels);
  }

  actionsOn(key: number): string {
    const names = (this.state?.bindings ?? [])
      .filter(b => b.device === InputDevice.Gamepad && b.key === key)
      .map(b => eventLabel(b.event));
    return names.length ? [...new Set(names)].join(', ') : '-';
  }

  keyName(device: InputDevice, key?: number): string {
    if (key === undefined || key === 0xff || key === 0xffff) return '-';
    if (device === InputDevice.Keyboard) return keyboardName(key);
    if (device === InputDevice.Mouse) return mouseName(key);
    return padName(key, this.model.labels);
  }

  isFocused(row: ActionRow): boolean {
    return this.focusKey !== undefined && row.gamepad === this.focusKey;
  }

  isCapturing(event: string, device: InputDevice): boolean {
    return this.capture?.event === event && this.capture.device === device;
  }

  startCapture(row: ActionRow, device: InputDevice): void {
    if (this.capture) this.client.cancelControlCapture();
    this.capture = { event: row.event, device };
    this.sound.play(Sound.Focus);
    this.client.startControlCapture(row.event, device);
  }

  cancelCapture(): void {
    if (!this.capture) return;
    this.capture = undefined;
    this.sound.play(Sound.Cancel);
    this.client.cancelControlCapture();
  }

  setMode(mode: 'controller' | 'keyboard'): void {
    this.cancelCapture();
    this.mode = mode;
    this.focusKey = undefined;
  }

  eventLabel(event: string): string {
    return eventLabel(event);
  }

  /** Escape is consumed natively while capturing; this covers the browser mock and focus quirks. */
  @HostListener('window:keydown.escape', ['$event'])
  onEscape(event: KeyboardEvent): void {
    if (!this.capture) return;
    this.cancelCapture();
    event.stopImmediatePropagation();
    event.preventDefault();
  }

  private buildActions(bindings: ControlBinding[]): ActionRow[] {
    const rows = new Map<string, ActionRow>();
    for (const binding of bindings) {
      if (!binding.remappable) continue;
      const row = rows.get(binding.event) ?? { event: binding.event, label: eventLabel(binding.event) };
      if (binding.device === InputDevice.Keyboard) row.keyboard = binding.key;
      else if (binding.device === InputDevice.Mouse) row.mouse = binding.key;
      else row.gamepad = binding.key;
      rows.set(binding.event, row);
    }
    const rank = (event: string) => {
      const index = ACTION_ORDER.indexOf(event);
      return index < 0 ? ACTION_ORDER.length : index;
    };
    return [...rows.values()].sort((a, b) => rank(a.event) - rank(b.event) || a.label.localeCompare(b.label));
  }
}
