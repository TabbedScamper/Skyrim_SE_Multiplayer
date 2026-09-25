import { Component, HostListener, OnDestroy, OnInit } from '@angular/core';
import { Subject, takeUntil } from 'rxjs';
import { ControlBinding, ControlBindingsState, ControllerFamily, InputDevice } from '../../models/control-bindings';
import { ClientService } from '../../services/client.service';
import { Sound, SoundService } from '../../services/sound.service';
import { eventLabel, keyboardName, mouseName, PAD, padName } from './key-names';

interface PadButton {
  key: number;
  side: 'left' | 'right';
  /** Label row (viewBox units). */
  labelY: number;
  shape: 'trigger' | 'bumper' | 'small' | 'face' | 'stick' | 'dpad';
  x: number;
  y: number;
}

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

  get family(): ControllerFamily {
    return this.state?.controller ?? 'xbox';
  }

  /** Stick and D-pad swap places on PlayStation pads. */
  get buttons(): PadButton[] {
    const ps = this.family === 'playstation';
    const dpad = ps ? { x: 220, y: 165 } : { x: 265, y: 240 };
    const leftStick = ps ? { x: 268, y: 240 } : { x: 220, y: 165 };
    return [
      { key: PAD.LeftTrigger, side: 'left', labelY: 40, shape: 'trigger', x: 215, y: 50 },
      { key: PAD.LeftBumper, side: 'left', labelY: 76, shape: 'bumper', x: 215, y: 80 },
      { key: PAD.Back, side: 'left', labelY: 112, shape: 'small', x: 290, y: 140 },
      { key: PAD.LeftStick, side: 'left', labelY: 148, shape: 'stick', ...leftStick },
      { key: PAD.DPadUp, side: 'left', labelY: 196, shape: 'dpad', x: dpad.x, y: dpad.y - 17 },
      { key: PAD.DPadLeft, side: 'left', labelY: 230, shape: 'dpad', x: dpad.x - 17, y: dpad.y },
      { key: PAD.DPadRight, side: 'left', labelY: 264, shape: 'dpad', x: dpad.x + 17, y: dpad.y },
      { key: PAD.DPadDown, side: 'left', labelY: 298, shape: 'dpad', x: dpad.x, y: dpad.y + 17 },
      { key: PAD.RightTrigger, side: 'right', labelY: 40, shape: 'trigger', x: 425, y: 50 },
      { key: PAD.RightBumper, side: 'right', labelY: 76, shape: 'bumper', x: 425, y: 80 },
      { key: PAD.Start, side: 'right', labelY: 112, shape: 'small', x: 350, y: 140 },
      { key: PAD.Y, side: 'right', labelY: 148, shape: 'face', x: 425, y: 138 },
      { key: PAD.X, side: 'right', labelY: 182, shape: 'face', x: 400, y: 163 },
      { key: PAD.B, side: 'right', labelY: 216, shape: 'face', x: 450, y: 163 },
      { key: PAD.A, side: 'right', labelY: 250, shape: 'face', x: 425, y: 188 },
      { key: PAD.RightStick, side: 'right', labelY: 298, shape: 'stick', x: 372, y: 240 },
    ];
  }

  /** Elbowed leader line from the button to its label column. */
  leader(button: PadButton): string {
    const labelX = button.side === 'left' ? 44 : 596;
    const elbowX = button.side === 'left' ? 72 : 568;
    return `${button.x},${button.y} ${elbowX},${button.labelY} ${labelX},${button.labelY}`;
  }

  padLabel(key: number): string {
    return padName(key, this.family);
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
    return padName(key, this.family);
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
