/** DirectInput scan codes (Skyrim keyboard bindings). E0-extended keys have the high bit set. */
const KEYBOARD: Record<number, string> = {
  0x01: 'Esc', 0x02: '1', 0x03: '2', 0x04: '3', 0x05: '4', 0x06: '5', 0x07: '6', 0x08: '7', 0x09: '8',
  0x0a: '9', 0x0b: '0', 0x0c: '-', 0x0d: '=', 0x0e: 'Backspace', 0x0f: 'Tab', 0x10: 'Q', 0x11: 'W',
  0x12: 'E', 0x13: 'R', 0x14: 'T', 0x15: 'Y', 0x16: 'U', 0x17: 'I', 0x18: 'O', 0x19: 'P', 0x1a: '[',
  0x1b: ']', 0x1c: 'Enter', 0x1d: 'Left Ctrl', 0x1e: 'A', 0x1f: 'S', 0x20: 'D', 0x21: 'F', 0x22: 'G',
  0x23: 'H', 0x24: 'J', 0x25: 'K', 0x26: 'L', 0x27: ';', 0x28: "'", 0x29: '`', 0x2a: 'Left Shift',
  0x2b: '\\', 0x2c: 'Z', 0x2d: 'X', 0x2e: 'C', 0x2f: 'V', 0x30: 'B', 0x31: 'N', 0x32: 'M', 0x33: ',',
  0x34: '.', 0x35: '/', 0x36: 'Right Shift', 0x37: 'Num *', 0x38: 'Left Alt', 0x39: 'Space',
  0x3a: 'Caps Lock', 0x3b: 'F1', 0x3c: 'F2', 0x3d: 'F3', 0x3e: 'F4', 0x3f: 'F5', 0x40: 'F6', 0x41: 'F7',
  0x42: 'F8', 0x43: 'F9', 0x44: 'F10', 0x45: 'Num Lock', 0x46: 'Scroll Lock', 0x47: 'Num 7', 0x48: 'Num 8',
  0x49: 'Num 9', 0x4a: 'Num -', 0x4b: 'Num 4', 0x4c: 'Num 5', 0x4d: 'Num 6', 0x4e: 'Num +', 0x4f: 'Num 1',
  0x50: 'Num 2', 0x51: 'Num 3', 0x52: 'Num 0', 0x53: 'Num .', 0x57: 'F11', 0x58: 'F12',
  0x9c: 'Num Enter', 0x9d: 'Right Ctrl', 0xb5: 'Num /', 0xb7: 'Print Screen', 0xb8: 'Right Alt',
  0xc5: 'Pause', 0xc7: 'Home', 0xc8: 'Up', 0xc9: 'Page Up', 0xcb: 'Left', 0xcd: 'Right', 0xcf: 'End',
  0xd0: 'Down', 0xd1: 'Page Down', 0xd2: 'Insert', 0xd3: 'Delete',
};

const MOUSE: Record<number, string> = {
  0: 'Left Mouse', 1: 'Right Mouse', 2: 'Middle Mouse', 3: 'Mouse 4', 4: 'Mouse 5',
  5: 'Mouse 6', 6: 'Mouse 7', 7: 'Mouse 8', 8: 'Wheel Up', 9: 'Wheel Down',
};

/** Skyrim gamepad codes: XInput button masks, triggers 9 and 10. */
export const PAD = {
  DPadUp: 0x0001, DPadDown: 0x0002, DPadLeft: 0x0004, DPadRight: 0x0008,
  Start: 0x0010, Back: 0x0020, LeftStick: 0x0040, RightStick: 0x0080,
  LeftBumper: 0x0100, RightBumper: 0x0200, A: 0x1000, B: 0x2000, X: 0x4000, Y: 0x8000,
  LeftTrigger: 9, RightTrigger: 10,
} as const;

const DPAD: Record<number, string> = {
  [PAD.DPadUp]: 'D-pad Up', [PAD.DPadDown]: 'D-pad Down', [PAD.DPadLeft]: 'D-pad Left', [PAD.DPadRight]: 'D-pad Right',
};

export function keyboardName(key: number): string {
  return KEYBOARD[key] ?? `Key ${key.toString(16).toUpperCase()}`;
}

export function mouseName(key: number): string {
  return MOUSE[key] ?? `Mouse ${key}`;
}

/** Name of a gamepad input using a model's own labels (D-pad names are shared). */
export function padName(key: number, labels: Record<number, string>): string {
  return DPAD[key] ?? labels[key] ?? `Button ${key}`;
}

/** Skyrim event IDs that read poorly as labels. */
const EVENT_LABELS: Record<string, string> = {
  'Tween Menu': 'Character Menu',
  'Ready Weapon': 'Draw / Sheathe',
  'Left Attack/Block': 'Left Hand',
  'Right Attack/Block': 'Right Hand',
  'Toggle POV': 'Camera View',
  'Quick Inventory': 'Inventory',
  'Quick Magic': 'Magic',
  'Quick Stats': 'Skills',
  'Quick Map': 'Map',
  'Auto-Move': 'Auto Move',
  'Toggle Always Run': 'Always Run',
};

export function eventLabel(event: string): string {
  const hotkey = /^Hotkey(\d)$/.exec(event);
  if (hotkey) return `Favorite ${hotkey[1]}`;
  return EVENT_LABELS[event] ?? event;
}
