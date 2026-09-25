import { PAD } from './key-names';

/** Detected by USB vendor/product ID in ControlBindings.cpp. */
export type ControllerModelId =
  | 'xbox-series' | 'xbox-one' | 'xbox-360'
  | 'dualsense' | 'dualshock4'
  | 'switch-pro'
  | 'steam-deck' | 'steam-controller'
  | 'generic';

export type ButtonShape = 'trigger' | 'bumper' | 'small' | 'face' | 'stick' | 'dpad';
/** How a face button is marked: a letter, or a PlayStation symbol. */
export type FaceGlyph = { letter: string } | { symbol: 'triangle' | 'circle' | 'cross' | 'square' };

export interface PadButton {
  key: number;
  side: 'left' | 'right';
  /** Label row in viewBox units. */
  labelY: number;
  shape: ButtonShape;
  x: number;
  y: number;
  glyph?: FaceGlyph;
}

/** Non-bindable detail drawn for recognition: touchpads, screens, guide buttons... */
export interface Decoration {
  kind: 'rect' | 'circle' | 'path';
  d?: string;
  x?: number;
  y?: number;
  w?: number;
  h?: number;
  r?: number;
  dashed?: boolean;
}

export interface ControllerModel {
  id: ControllerModelId;
  name: string;
  body: string[];
  decorations: Decoration[];
  buttons: PadButton[];
  labels: Record<number, string>;
  note?: string;
}

// Shared coordinate frame: viewBox "-120 0 880 345"; callout columns at x=40 (left) and x=600 (right).
const XBOX_BODY =
  'M205,92 C250,84 290,96 320,96 C350,96 390,84 435,92 C485,100 505,140 522,205 C540,270 548,330 505,338 ' +
  'C470,344 448,300 425,272 L215,272 C192,300 170,344 135,338 C92,330 100,270 118,205 C135,140 155,100 205,92 Z';
const XBOX_CONTOUR =
  'M210,104 C252,97 290,108 320,108 C350,108 388,97 430,104 C474,112 492,148 507,205 C522,262 528,318 500,324 ' +
  'C476,328 456,294 432,262 L208,262 C184,294 164,328 140,324 C112,318 118,262 133,205 C148,148 166,112 210,104 Z';

const LETTERS = (a: string, b: string, x: string, y: string) => ({
  [PAD.A]: { letter: a }, [PAD.B]: { letter: b }, [PAD.X]: { letter: x }, [PAD.Y]: { letter: y },
});

/** Offset-layout pad (Xbox, Switch Pro): left stick high, D-pad low. */
function offsetButtons(glyphs: Record<number, FaceGlyph>, faceX = 425, faceY = 163): PadButton[] {
  const face = (key: number, dx: number, dy: number, labelY: number): PadButton =>
    ({ key, side: 'right', labelY, shape: 'face', x: faceX + dx, y: faceY + dy, glyph: glyphs[key] });
  return [
    { key: PAD.LeftTrigger, side: 'left', labelY: 40, shape: 'trigger', x: 215, y: 50 },
    { key: PAD.LeftBumper, side: 'left', labelY: 76, shape: 'bumper', x: 215, y: 80 },
    { key: PAD.Back, side: 'left', labelY: 112, shape: 'small', x: 288, y: 140 },
    { key: PAD.LeftStick, side: 'left', labelY: 148, shape: 'stick', x: 220, y: 165 },
    { key: PAD.DPadUp, side: 'left', labelY: 196, shape: 'dpad', x: 265, y: 223 },
    { key: PAD.DPadLeft, side: 'left', labelY: 230, shape: 'dpad', x: 248, y: 240 },
    { key: PAD.DPadRight, side: 'left', labelY: 264, shape: 'dpad', x: 282, y: 240 },
    { key: PAD.DPadDown, side: 'left', labelY: 298, shape: 'dpad', x: 265, y: 257 },
    { key: PAD.RightTrigger, side: 'right', labelY: 40, shape: 'trigger', x: 425, y: 50 },
    { key: PAD.RightBumper, side: 'right', labelY: 76, shape: 'bumper', x: 425, y: 80 },
    { key: PAD.Start, side: 'right', labelY: 112, shape: 'small', x: 352, y: 140 },
    face(PAD.Y, 0, -26, 148),
    face(PAD.X, -26, 0, 182),
    face(PAD.B, 26, 0, 216),
    face(PAD.A, 0, 26, 250),
    { key: PAD.RightStick, side: 'right', labelY: 298, shape: 'stick', x: 372, y: 240 },
  ];
}

const XBOX_LABELS = (back: string, start: string): Record<number, string> => ({
  [PAD.A]: 'A', [PAD.B]: 'B', [PAD.X]: 'X', [PAD.Y]: 'Y',
  [PAD.LeftBumper]: 'LB', [PAD.RightBumper]: 'RB', [PAD.LeftTrigger]: 'LT', [PAD.RightTrigger]: 'RT',
  [PAD.LeftStick]: 'LS', [PAD.RightStick]: 'RS', [PAD.Back]: back, [PAD.Start]: start,
});

function xbox(id: ControllerModelId, name: string, back: string, start: string, share: boolean): ControllerModel {
  return {
    id, name,
    body: [XBOX_BODY],
    decorations: [
      { kind: 'path', d: XBOX_CONTOUR, dashed: true },
      { kind: 'circle', x: 320, y: 118, r: 11 }, // guide
      ...(share ? [{ kind: 'rect', x: 313, y: 150, w: 14, h: 9 } as Decoration] : []),
      { kind: 'path', d: 'M130,300 C140,320 150,330 160,332 M510,300 C500,320 490,330 480,332', dashed: true },
    ],
    buttons: offsetButtons(LETTERS('A', 'B', 'X', 'Y')),
    labels: XBOX_LABELS(back, start),
  };
}

// ---- PlayStation: symmetric sticks low, D-pad and face high, touchpad in the middle ----
const PS_BODY =
  'M195,96 C245,88 285,104 320,104 C355,104 395,88 445,96 C500,104 520,150 536,215 C552,285 548,338 506,342 ' +
  'C476,345 454,308 432,278 L208,278 C186,308 164,345 134,342 C92,338 88,285 104,215 C120,150 140,104 195,96 Z';

function playstation(id: ControllerModelId, name: string, back: string): ControllerModel {
  const face = (key: number, x: number, y: number, labelY: number, symbol: 'triangle' | 'circle' | 'cross' | 'square'): PadButton =>
    ({ key, side: 'right', labelY, shape: 'face', x, y, glyph: { symbol } });
  return {
    id, name,
    body: [PS_BODY],
    decorations: [
      { kind: 'rect', x: 266, y: 106, w: 108, h: 64 },  // touchpad
      { kind: 'path', d: 'M270,176 L370,176', dashed: true }, // light bar
      { kind: 'circle', x: 320, y: 206, r: 8 },  // PS button
      { kind: 'rect', x: 312, y: 222, w: 16, h: 5 }, // mic mute
      { kind: 'path', d: 'M118,300 C130,322 142,332 154,334 M522,300 C510,322 498,332 486,334', dashed: true },
    ],
    buttons: [
      { key: PAD.LeftTrigger, side: 'left', labelY: 40, shape: 'trigger', x: 212, y: 52 },
      { key: PAD.LeftBumper, side: 'left', labelY: 76, shape: 'bumper', x: 212, y: 82 },
      { key: PAD.Back, side: 'left', labelY: 112, shape: 'small', x: 252, y: 114 },
      { key: PAD.DPadUp, side: 'left', labelY: 148, shape: 'dpad', x: 200, y: 148 },
      { key: PAD.DPadLeft, side: 'left', labelY: 182, shape: 'dpad', x: 183, y: 165 },
      { key: PAD.DPadRight, side: 'left', labelY: 216, shape: 'dpad', x: 217, y: 165 },
      { key: PAD.DPadDown, side: 'left', labelY: 250, shape: 'dpad', x: 200, y: 182 },
      { key: PAD.LeftStick, side: 'left', labelY: 298, shape: 'stick', x: 266, y: 240 },
      { key: PAD.RightTrigger, side: 'right', labelY: 40, shape: 'trigger', x: 428, y: 52 },
      { key: PAD.RightBumper, side: 'right', labelY: 76, shape: 'bumper', x: 428, y: 82 },
      { key: PAD.Start, side: 'right', labelY: 112, shape: 'small', x: 388, y: 114 },
      face(PAD.Y, 440, 140, 148, 'triangle'),
      face(PAD.X, 414, 166, 182, 'square'),
      face(PAD.B, 466, 166, 216, 'circle'),
      face(PAD.A, 440, 192, 250, 'cross'),
      { key: PAD.RightStick, side: 'right', labelY: 298, shape: 'stick', x: 374, y: 240 },
    ],
    labels: {
      [PAD.A]: 'Cross', [PAD.B]: 'Circle', [PAD.X]: 'Square', [PAD.Y]: 'Triangle',
      [PAD.LeftBumper]: 'L1', [PAD.RightBumper]: 'R1', [PAD.LeftTrigger]: 'L2', [PAD.RightTrigger]: 'R2',
      [PAD.LeftStick]: 'L3', [PAD.RightStick]: 'R3', [PAD.Back]: back, [PAD.Start]: 'Options',
    },
  };
}

// ---- Nintendo Switch Pro: offset layout, face letters swapped by position ----
const SWITCH_BODY =
  'M200,94 C250,86 290,98 320,98 C350,98 390,86 440,94 C492,102 512,146 528,210 C544,276 540,334 500,338 ' +
  'C470,341 450,304 428,276 L212,276 C190,304 170,341 140,338 C100,334 96,276 112,210 C128,146 148,102 200,94 Z';

const switchPro: ControllerModel = {
  id: 'switch-pro', name: 'Nintendo Switch Pro Controller',
  body: [SWITCH_BODY],
  decorations: [
    { kind: 'circle', x: 344, y: 168, r: 7 }, // home
    { kind: 'rect', x: 290, y: 162, w: 12, h: 12 }, // capture
    { kind: 'path', d: 'M135,300 C145,320 155,330 165,332 M505,300 C495,320 485,330 475,332', dashed: true },
  ],
  // Skyrim's A (bottom) is Nintendo's B, and so on round the diamond.
  buttons: offsetButtons(LETTERS('B', 'A', 'Y', 'X'), 420, 160),
  labels: {
    [PAD.A]: 'B', [PAD.B]: 'A', [PAD.X]: 'Y', [PAD.Y]: 'X',
    [PAD.LeftBumper]: 'L', [PAD.RightBumper]: 'R', [PAD.LeftTrigger]: 'ZL', [PAD.RightTrigger]: 'ZR',
    [PAD.LeftStick]: 'LS', [PAD.RightStick]: 'RS', [PAD.Back]: '-', [PAD.Start]: '+',
  },
};

// ---- Steam Deck: handheld, screen in the middle, trackpads below the sticks ----
const DECK_BODY =
  'M58,112 C58,94 74,86 96,86 L544,86 C566,86 582,94 582,112 L594,248 C598,298 578,330 542,330 ' +
  'C508,330 492,302 470,292 L170,292 C148,302 132,330 98,330 C62,330 42,298 46,248 Z';

const steamDeck: ControllerModel = {
  id: 'steam-deck', name: 'Steam Deck',
  body: [DECK_BODY],
  decorations: [
    { kind: 'rect', x: 190, y: 100, w: 260, h: 164 }, // screen
    { kind: 'rect', x: 198, y: 108, w: 244, h: 148, dashed: true },
    { kind: 'rect', x: 96, y: 200, w: 62, h: 62 },  // left trackpad
    { kind: 'rect', x: 482, y: 200, w: 62, h: 62 }, // right trackpad
    { kind: 'rect', x: 72, y: 272, w: 14, h: 8 },   // steam button
    { kind: 'circle', x: 560, y: 276, r: 5 },       // quick access
  ],
  buttons: [
    { key: PAD.LeftTrigger, side: 'left', labelY: 40, shape: 'trigger', x: 115, y: 58 },
    { key: PAD.LeftBumper, side: 'left', labelY: 76, shape: 'bumper', x: 115, y: 80 },
    { key: PAD.Back, side: 'left', labelY: 112, shape: 'small', x: 176, y: 104 },
    { key: PAD.LeftStick, side: 'left', labelY: 148, shape: 'stick', x: 160, y: 150 },
    { key: PAD.DPadUp, side: 'left', labelY: 182, shape: 'dpad', x: 94, y: 128 },
    { key: PAD.DPadLeft, side: 'left', labelY: 216, shape: 'dpad', x: 77, y: 145 },
    { key: PAD.DPadRight, side: 'left', labelY: 250, shape: 'dpad', x: 111, y: 145 },
    { key: PAD.DPadDown, side: 'left', labelY: 284, shape: 'dpad', x: 94, y: 162 },
    { key: PAD.RightTrigger, side: 'right', labelY: 40, shape: 'trigger', x: 525, y: 58 },
    { key: PAD.RightBumper, side: 'right', labelY: 76, shape: 'bumper', x: 525, y: 80 },
    { key: PAD.Start, side: 'right', labelY: 112, shape: 'small', x: 464, y: 104 },
    { key: PAD.Y, side: 'right', labelY: 148, shape: 'face', x: 546, y: 120, glyph: { letter: 'Y' } },
    { key: PAD.X, side: 'right', labelY: 182, shape: 'face', x: 522, y: 144, glyph: { letter: 'X' } },
    { key: PAD.B, side: 'right', labelY: 216, shape: 'face', x: 570, y: 144, glyph: { letter: 'B' } },
    { key: PAD.A, side: 'right', labelY: 250, shape: 'face', x: 546, y: 168, glyph: { letter: 'A' } },
    { key: PAD.RightStick, side: 'right', labelY: 284, shape: 'stick', x: 480, y: 150 },
  ],
  labels: {
    [PAD.A]: 'A', [PAD.B]: 'B', [PAD.X]: 'X', [PAD.Y]: 'Y',
    [PAD.LeftBumper]: 'L1', [PAD.RightBumper]: 'R1', [PAD.LeftTrigger]: 'L2', [PAD.RightTrigger]: 'R2',
    [PAD.LeftStick]: 'L3', [PAD.RightStick]: 'R3', [PAD.Back]: 'View', [PAD.Start]: 'Menu',
  },
  note: 'Back grips (L4, L5, R4, R5) and trackpads are set up in Steam Input.',
};

const steamController: ControllerModel = {
  ...xbox('steam-controller', 'Steam Controller', 'Back', 'Start', false),
  note: 'Trackpads and back grips are set up in Steam Input.',
};

export const CONTROLLER_MODELS: Record<ControllerModelId, ControllerModel> = {
  'xbox-series': xbox('xbox-series', 'Xbox Series X|S Controller', 'View', 'Menu', true),
  'xbox-one': xbox('xbox-one', 'Xbox One Controller', 'View', 'Menu', false),
  'xbox-360': xbox('xbox-360', 'Xbox 360 Controller', 'Back', 'Start', false),
  dualsense: playstation('dualsense', 'DualSense (PS5)', 'Create'),
  dualshock4: playstation('dualshock4', 'DualShock 4 (PS4)', 'Share'),
  'switch-pro': switchPro,
  'steam-deck': steamDeck,
  'steam-controller': steamController,
  generic: { ...xbox('generic', 'Controller', 'Back', 'Start', false) },
};

export const MODEL_CHOICES: { id: ControllerModelId; name: string }[] =
  (Object.keys(CONTROLLER_MODELS) as ControllerModelId[]).map(id => ({ id, name: CONTROLLER_MODELS[id].name }));
