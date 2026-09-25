export enum DisplayMode {
  Windowed = 0,
  Borderless = 1,
  Fullscreen = 2,
}

export interface GameSettings {
  displayMode: DisplayMode;
  monitor: number;
  width: number;
  height: number;
  vsync: boolean;
  master: number;
  footsteps: number;
  voice: number;
  music: number;
  effects: number;
  gamma: number;
  mouseSensitivity: number;
  gamepadSensitivity: number;
  invertY: boolean;
  dialogueSubtitles: boolean;
  generalSubtitles: boolean;
  alwaysRun: boolean;
  controllerRumble: boolean;
  /** Windows endpoint ID of the chosen output; empty = follow the Windows default. */
  audioDevice: string;
}

export interface AudioDevice {
  id: string;
  name: string;
}

export interface GameSettingsPayload {
  settings: GameSettings;
  monitors: string[];
  resolutions: string[];
  /** Active output devices; undefined when the game client does not report them. */
  audioDevices?: AudioDevice[];
  defaults: boolean;
}
