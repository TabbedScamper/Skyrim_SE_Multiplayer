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
}

export interface GameSettingsPayload {
  settings: GameSettings;
  monitors: string[];
  resolutions: string[];
  defaults: boolean;
}
