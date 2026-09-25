import { Component } from '@angular/core';
import { GamepadNavigationService, NavPrompt } from '../../services/gamepad-navigation.service';

type Glyph =
  | { kind: 'face'; text?: string; symbol?: 'cross' | 'circle' | 'square' | 'triangle' }
  | { kind: 'pill'; text: string }
  | { kind: 'dpad-lr' }
  | { kind: 'stick'; text: string };

/** Skyrim-style button prompt bar for controller navigation (bottom of the screen). */
@Component({
  selector: 'app-gamepad-prompts',
  templateUrl: './gamepad-prompts.component.html',
  styleUrls: ['./gamepad-prompts.component.scss'],
})
export class GamepadPromptsComponent {
  constructor(readonly nav: GamepadNavigationService) {}

  trackPrompt(_: number, prompt: NavPrompt): string {
    return prompt.button + prompt.label;
  }

  /** Glyphs for a prompt on the detected controller (one prompt can show two, e.g. LB/RB). */
  glyphs(button: string, model: string | null): Glyph[] {
    const ps = model === 'dualsense' || model === 'dualshock4';
    const nintendo = model === 'switch-pro';
    const deck = model === 'steam-deck';
    switch (button) {
      case 'a':
        return [ps ? { kind: 'face', symbol: 'cross' } : { kind: 'face', text: nintendo ? 'B' : 'A' }];
      case 'b':
        return [ps ? { kind: 'face', symbol: 'circle' } : { kind: 'face', text: nintendo ? 'A' : 'B' }];
      case 'x':
        return [ps ? { kind: 'face', symbol: 'square' } : { kind: 'face', text: nintendo ? 'Y' : 'X' }];
      case 'y':
        return [ps ? { kind: 'face', symbol: 'triangle' } : { kind: 'face', text: nintendo ? 'X' : 'Y' }];
      case 'lbrb':
        return ps || deck ? [{ kind: 'pill', text: 'L1' }, { kind: 'pill', text: 'R1' }]
          : nintendo ? [{ kind: 'pill', text: 'L' }, { kind: 'pill', text: 'R' }]
          : [{ kind: 'pill', text: 'LB' }, { kind: 'pill', text: 'RB' }];
      case 'start':
        return [{ kind: 'pill', text: ps ? 'Options' : nintendo ? '+' : 'Menu' }];
      case 'view':
        return [{ kind: 'pill', text: model === 'dualsense' ? 'Create' : model === 'dualshock4' ? 'Share' : nintendo ? '−' : 'View' }];
      case 'lr':
        return [{ kind: 'dpad-lr' }];
      case 'rs':
        return [{ kind: 'stick', text: ps ? 'R' : 'RS' }];
      default:
        return [{ kind: 'pill', text: button.toUpperCase() }];
    }
  }
}
