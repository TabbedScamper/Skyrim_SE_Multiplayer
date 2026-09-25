export type ControllerFamily = 'xbox' | 'playstation' | 'nintendo' | 'none';

/** INPUT_DEVICE as Skyrim's ControlMap numbers it. */
export enum InputDevice {
  Keyboard = 0,
  Mouse = 1,
  Gamepad = 2,
}

export interface ControlBinding {
  /** Skyrim user event ID, e.g. "Activate", "Ready Weapon". */
  event: string;
  device: InputDevice;
  /** DirectInput scan code, mouse button index, or XInput mask (LT 9, RT 10). */
  key: number;
  remappable: boolean;
}

export interface ControlBindingsState {
  controller: ControllerFamily;
  bindings: ControlBinding[];
}
