import { CommonModule } from '@angular/common';
import { Component, NgZone, OnDestroy } from '@angular/core';

type Glyph =
  | { kind: 'face'; text?: string; symbol?: 'cross' | 'circle' | 'square' | 'triangle' }
  | { kind: 'pill'; text: string }
  | { kind: 'key'; text: string };

/**
 * Revive prompt and meter (ReviveService, client). Modes: 0 hidden; 1 a downed player in reach
 * ("[button] Revive" under their name, like Skyrim's activation prompt); 2 reviving them (meter);
 * 3 this player is down (the bleedout meter, and the revive meter while someone revives them);
 * 4 this player has fallen and spectates (name watched, previous/next); 5 a fallen ally can be called back
 * ("[Shout] Hold: Call back" with the ritual's progress); 6 the whole party has fallen (before the reload).
 * The button is drawn for the device used last: the keyboard key, or the controller's glyph
 * (Xbox letters, PlayStation symbols, Nintendo's swapped letters).
 */
@Component({
  selector: 'app-revive-prompt',
  standalone: true,
  imports: [CommonModule],
  templateUrl: './revive-prompt.component.html',
  styleUrls: ['./revive-prompt.component.scss'],
})
export class RevivePromptComponent implements OnDestroy {
  mode = 0;
  name = '';
  key = '';
  progress = 0;
  hint = '';
  note = '';
  button = '';
  model = 'xbox-series';
  gamepad = false;
  /** Bleedout meter, 1 to 0 over two minutes (-1: not known). */
  bleed = -1;

  private readonly receive: SkyrimTogetherTypes.ReviveCallback = (mode, name, key, progress, hint, note, button, model, gamepad, bleed) =>
    this.zone.run(() => {
      this.mode = mode;
      this.name = name;
      this.key = key;
      this.progress = Math.max(0, Math.min(1, progress));
      this.hint = hint;
      this.note = note;
      this.button = button;
      this.model = model || 'xbox-series';
      this.gamepad = gamepad;
      this.bleed = bleed < 0 ? -1 : Math.min(1, bleed);
    });

  constructor(private readonly zone: NgZone) {
    skyrimtogether.on('revive', this.receive);
  }

  ngOnDestroy(): void {
    skyrimtogether.off('revive', this.receive);
  }

  /** Keyboard keys for previous/next while spectating ("A|D"). */
  spectateKeys(): string[] {
    const keys = (this.key || 'A|D').split('|');
    return [keys[0] ?? 'A', keys[1] ?? 'D'];
  }

  /** Time left on the bleedout meter, m:ss (the meter drains over two minutes). */
  timeLeft(): string {
    const seconds = Math.max(0, Math.ceil(Math.max(0, this.bleed) * 120));
    return `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, '0')}`;
  }

  /** The Activate glyph for the device in use (same glyph sets as the gamepad prompt bar). */
  glyph(): Glyph | null {
    if (!this.gamepad || !this.button) {
      return this.key ? { kind: 'key', text: this.key } : null;
    }
    const ps = this.model === 'dualsense' || this.model === 'dualshock4';
    const nintendo = this.model === 'switch-pro';
    switch (this.button) {
      case 'a':
        return ps ? { kind: 'face', symbol: 'cross' } : { kind: 'face', text: nintendo ? 'B' : 'A' };
      case 'b':
        return ps ? { kind: 'face', symbol: 'circle' } : { kind: 'face', text: nintendo ? 'A' : 'B' };
      case 'x':
        return ps ? { kind: 'face', symbol: 'square' } : { kind: 'face', text: nintendo ? 'Y' : 'X' };
      case 'y':
        return ps ? { kind: 'face', symbol: 'triangle' } : { kind: 'face', text: nintendo ? 'X' : 'Y' };
      case 'lb':
        return { kind: 'pill', text: ps ? 'L1' : nintendo ? 'L' : 'LB' };
      case 'rb':
        return { kind: 'pill', text: ps ? 'R1' : nintendo ? 'R' : 'RB' };
      case 'lt':
        return { kind: 'pill', text: ps ? 'L2' : nintendo ? 'ZL' : 'LT' };
      case 'rt':
        return { kind: 'pill', text: ps ? 'R2' : nintendo ? 'ZR' : 'RT' };
      default:
        return { kind: 'pill', text: this.button.toUpperCase() };
    }
  }
}
