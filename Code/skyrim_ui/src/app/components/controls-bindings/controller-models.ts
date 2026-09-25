import { PAD } from './key-names';

/** Detected by USB vendor/product ID in ControlBindings.cpp. */
export type ControllerModelId =
  | 'xbox-series' | 'xbox-one' | 'xbox-360'
  | 'dualsense' | 'dualshock4'
  | 'switch-pro'
  | 'steam-deck' | 'steam-controller'
  | 'generic';

export interface PadButton {
  key: number;
  side: 'left' | 'right';
  /** Label row in viewBox units. */
  labelY: number;
  /** Centre of the button on the art, in viewBox units. */
  x: number;
  y: number;
  /** Half size of the button's highlight ring. */
  rx: number;
  ry: number;
}

export interface ControllerModel {
  id: ControllerModelId;
  name: string;
  /** Front-view art (assets/images/controllers, see NOTICE.md there). */
  art: { src: string; x: number; y: number; width: number; height: number };
  buttons: PadButton[];
  labels: Record<number, string>;
  note?: string;
}

/** Button centre and size [cx, cy, w, h] in the art file's own units. */
type Anchor = [number, number, number, number];

// Shared frame: viewBox "-120 0 880 345"; callout columns at x=40 (left) and
// x=600 (right); the art is fitted inside ART_BOX between them.
const ART_BOX = { x: 84, y: 4, width: 472, height: 337 };

const LEFT_KEYS: number[] = [PAD.LeftTrigger, PAD.LeftBumper, PAD.Back, PAD.LeftStick, PAD.DPadUp, PAD.DPadLeft, PAD.DPadRight, PAD.DPadDown];

/** D-pad arms around a centre, `reach` from it, each `size` square. */
function dpad(cx: number, cy: number, reach: number, size: number): Record<number, Anchor> {
  return {
    [PAD.DPadUp]: [cx, cy - reach, size, size],
    [PAD.DPadDown]: [cx, cy + reach, size, size],
    [PAD.DPadLeft]: [cx - reach, cy, size, size],
    [PAD.DPadRight]: [cx + reach, cy, size, size],
  };
}

/**
 * Places the art in the frame and maps each anchor onto it. Anchors were
 * measured from the art's own labelled layers (e.g. "A Button", "Left Stick").
 */
function fromArt(
  id: ControllerModelId, name: string, file: string, size: [number, number],
  anchors: Record<number, Anchor>, labels: Record<number, string>, note?: string,
): ControllerModel {
  const [w, h] = size;
  const scale = Math.min(ART_BOX.width / w, ART_BOX.height / h);
  const ox = ART_BOX.x + (ART_BOX.width - w * scale) / 2;
  const oy = ART_BOX.y + (ART_BOX.height - h * scale) / 2;
  const buttons = Object.entries(anchors).map(([key, [cx, cy, bw, bh]]): PadButton => {
    const x = ox + cx * scale, y = oy + cy * scale;
    return {
      key: +key,
      side: LEFT_KEYS.includes(+key) ? 'left' : 'right',
      labelY: y,
      x, y,
      rx: Math.max(7, (bw * scale) / 2 + 2),
      ry: Math.max(7, (bh * scale) / 2 + 2),
    };
  });
  return {
    id, name, note, buttons, labels,
    art: { src: `assets/images/controllers/${file}.svg`, x: ox, y: oy, width: w * scale, height: h * scale },
  };
}

const XBOX_LABELS = (back: string, start: string): Record<number, string> => ({
  [PAD.A]: 'A', [PAD.B]: 'B', [PAD.X]: 'X', [PAD.Y]: 'Y',
  [PAD.LeftBumper]: 'LB', [PAD.RightBumper]: 'RB', [PAD.LeftTrigger]: 'LT', [PAD.RightTrigger]: 'RT',
  [PAD.LeftStick]: 'LS', [PAD.RightStick]: 'RS', [PAD.Back]: back, [PAD.Start]: start,
});

const PS_LABELS = (back: string): Record<number, string> => ({
  [PAD.A]: 'Cross', [PAD.B]: 'Circle', [PAD.X]: 'Square', [PAD.Y]: 'Triangle',
  [PAD.LeftBumper]: 'L1', [PAD.RightBumper]: 'R1', [PAD.LeftTrigger]: 'L2', [PAD.RightTrigger]: 'R2',
  [PAD.LeftStick]: 'L3', [PAD.RightStick]: 'R3', [PAD.Back]: back, [PAD.Start]: 'Options',
});

const xboxSeries = (id: ControllerModelId, name: string) => fromArt(id, name, 'xbox-series', [1534.7, 954], {
  [PAD.LeftTrigger]: [306, 65, 169, 129], [PAD.RightTrigger]: [1216, 57, 166, 114],
  [PAD.LeftBumper]: [345, 140, 240, 50], [PAD.RightBumper]: [1190, 140, 240, 50],
  [PAD.Back]: [650, 420, 76, 70], [PAD.Start]: [884, 420, 76, 70],
  [PAD.LeftStick]: [352, 443, 177, 176], [PAD.RightStick]: [976, 671, 180, 170],
  ...dpad(558, 660, 62, 72),
  [PAD.Y]: [1183, 311, 119, 110], [PAD.X]: [1071, 418, 114, 108], [PAD.B]: [1286, 407, 112, 107], [PAD.A]: [1177, 514, 110, 106],
}, XBOX_LABELS('View', 'Menu'));

const xboxOne = (id: ControllerModelId, name: string, back: string, start: string) => fromArt(id, name, 'xbox-one', [1543.2, 956.3], {
  [PAD.LeftTrigger]: [291, 96, 206, 188], [PAD.RightTrigger]: [1254, 95, 208, 189],
  [PAD.LeftBumper]: [330, 215, 240, 50], [PAD.RightBumper]: [1216, 215, 240, 50],
  [PAD.Back]: [647, 495, 76, 70], [PAD.Start]: [894, 495, 76, 70],
  [PAD.LeftStick]: [345, 512, 181, 180], [PAD.RightStick]: [985, 746, 186, 176],
  ...dpad(551, 720, 62, 72),
  [PAD.Y]: [1217, 388, 119, 110], [PAD.X]: [1092, 493, 120, 108], [PAD.B]: [1327, 480, 115, 107], [PAD.A]: [1203, 587, 115, 107],
}, XBOX_LABELS(back, start));

const xbox360 = fromArt('xbox-360', 'Xbox 360 Controller', 'xbox-360', [408.8, 252.8], {
  [PAD.LeftTrigger]: [92, 19, 36, 39], [PAD.RightTrigger]: [325, 19, 36, 39],
  [PAD.LeftBumper]: [78, 53, 81, 36], [PAD.RightBumper]: [336, 53, 74, 36],
  [PAD.Back]: [157, 125, 29, 24], [PAD.Start]: [254, 125, 30, 23],
  [PAD.LeftStick]: [79, 141, 54, 48], [PAD.RightStick]: [264, 203, 54, 46],
  ...dpad(142, 190, 18, 20),
  [PAD.Y]: [332, 99, 33, 30], [PAD.X]: [297, 127, 32, 28], [PAD.B]: [363, 125, 31, 29], [PAD.A]: [328, 154, 32, 26],
}, XBOX_LABELS('Back', 'Start'));

const dualsense = fromArt('dualsense', 'DualSense (PS5)', 'dualsense', [544.7, 302.9], {
  [PAD.LeftTrigger]: [101, 29, 75, 57], [PAD.RightTrigger]: [444, 28, 75, 55],
  [PAD.LeftBumper]: [97, 55, 81, 46], [PAD.RightBumper]: [448, 55, 81, 46],
  [PAD.Back]: [140, 102, 18, 33], [PAD.Start]: [405, 102, 18, 34],
  [PAD.LeftStick]: [183, 235, 65, 55], [PAD.RightStick]: [362, 236, 65, 55],
  [PAD.DPadUp]: [97, 137, 32, 37], [PAD.DPadLeft]: [70, 160, 39, 30], [PAD.DPadDown]: [97, 181, 32, 35], [PAD.DPadRight]: [124, 160, 39, 30],
  [PAD.Y]: [449, 125, 38, 35], [PAD.X]: [407, 160, 38, 33], [PAD.A]: [447, 193, 38, 31], [PAD.B]: [489, 158, 38, 35],
}, PS_LABELS('Create'));

const dualshock4 = fromArt('dualshock4', 'DualShock 4 (PS4)', 'dualshock4', [1542.6, 824.3], {
  [PAD.LeftTrigger]: [315, 50, 172, 99], [PAD.RightTrigger]: [1228, 50, 173, 99],
  [PAD.LeftBumper]: [310, 127, 203, 97], [PAD.RightBumper]: [1233, 127, 203, 97],
  [PAD.Back]: [466, 270, 74, 112], [PAD.Start]: [1077, 270, 100, 112],
  [PAD.LeftStick]: [533, 629, 173, 155], [PAD.RightStick]: [1009, 629, 173, 155],
  [PAD.DPadUp]: [306, 344, 90, 120], [PAD.DPadRight]: [393, 421, 140, 85], [PAD.DPadDown]: [307, 496, 90, 128], [PAD.DPadLeft]: [221, 421, 138, 85],
  [PAD.Y]: [1236, 324, 105, 94], [PAD.X]: [1127, 420, 105, 94], [PAD.B]: [1346, 420, 105, 94], [PAD.A]: [1236, 515, 105, 94],
}, PS_LABELS('Share'));

// Skyrim's A (bottom) is Nintendo's B, and so on round the diamond.
const switchPro = fromArt('switch-pro', 'Nintendo Switch Pro Controller', 'switch-pro', [419.1, 304.5], {
  [PAD.LeftTrigger]: [96, 14, 74, 27], [PAD.RightTrigger]: [323, 14, 74, 27],
  [PAD.LeftBumper]: [99, 31, 102, 24], [PAD.RightBumper]: [320, 31, 102, 24],
  [PAD.Back]: [158, 66, 19, 19], [PAD.Start]: [261, 66, 18, 18],
  [PAD.LeftStick]: [94, 97, 49, 49], [PAD.RightStick]: [264, 155, 50, 51],
  ...dpad(146, 155, 19, 20),
  [PAD.Y]: [320, 68, 30, 30], [PAD.X]: [287, 97, 30, 30], [PAD.B]: [353, 97, 30, 30], [PAD.A]: [320, 126, 30, 30],
}, {
  [PAD.A]: 'B', [PAD.B]: 'A', [PAD.X]: 'Y', [PAD.Y]: 'X',
  [PAD.LeftBumper]: 'L', [PAD.RightBumper]: 'R', [PAD.LeftTrigger]: 'ZL', [PAD.RightTrigger]: 'ZR',
  [PAD.LeftStick]: 'LS', [PAD.RightStick]: 'RS', [PAD.Back]: '-', [PAD.Start]: '+',
});

// Front view: the triggers sit behind the shoulder buttons along the top edge.
const steamDeck = fromArt('steam-deck', 'Steam Deck', 'steam-deck', [492.7, 200.9], {
  [PAD.LeftTrigger]: [26, 7, 22, 8], [PAD.RightTrigger]: [466, 7, 22, 8],
  [PAD.LeftBumper]: [62, 9, 34, 8], [PAD.RightBumper]: [430, 9, 34, 8],
  [PAD.Back]: [57, 20, 15, 6], [PAD.Start]: [435, 20, 15, 6],
  [PAD.LeftStick]: [78, 48, 33, 33], [PAD.RightStick]: [415, 48, 33, 33],
  ...dpad(31, 40, 11, 12),
  [PAD.Y]: [463, 25, 15, 15], [PAD.X]: [448, 40, 15, 15], [PAD.B]: [477, 40, 15, 15], [PAD.A]: [463, 54, 15, 15],
}, {
  [PAD.A]: 'A', [PAD.B]: 'B', [PAD.X]: 'X', [PAD.Y]: 'Y',
  [PAD.LeftBumper]: 'L1', [PAD.RightBumper]: 'R1', [PAD.LeftTrigger]: 'L2', [PAD.RightTrigger]: 'R2',
  [PAD.LeftStick]: 'L3', [PAD.RightStick]: 'R3', [PAD.Back]: 'View', [PAD.Start]: 'Menu',
}, 'Back grips (L4, L5, R4, R5) and trackpads are set up in Steam Input.');

// Skyrim sees the left trackpad as the D-pad and the right one as the right stick.
const steamController = fromArt('steam-controller', 'Steam Controller', 'steam-controller', [416.8, 298.2], {
  [PAD.LeftTrigger]: [88, 8, 51, 13], [PAD.RightTrigger]: [329, 8, 51, 13],
  [PAD.LeftBumper]: [94, 17, 78, 10], [PAD.RightBumper]: [322, 17, 77, 10],
  [PAD.Back]: [174, 88, 25, 14], [PAD.Start]: [242, 88, 25, 14],
  [PAD.LeftStick]: [161, 151, 67, 67], [PAD.RightStick]: [322, 89, 106, 104],
  ...dpad(94, 90, 27, 26),
  [PAD.Y]: [257, 127, 23, 23], [PAD.X]: [234, 150, 23, 23], [PAD.B]: [281, 150, 23, 23], [PAD.A]: [257, 173, 23, 23],
}, XBOX_LABELS('Back', 'Start'), 'Trackpads and back grips are set up in Steam Input.');

export const CONTROLLER_MODELS: Record<ControllerModelId, ControllerModel> = {
  'xbox-series': xboxSeries('xbox-series', 'Xbox Series X|S Controller'),
  'xbox-one': xboxOne('xbox-one', 'Xbox One Controller', 'View', 'Menu'),
  'xbox-360': xbox360,
  dualsense,
  dualshock4,
  'switch-pro': switchPro,
  'steam-deck': steamDeck,
  'steam-controller': steamController,
  generic: xboxOne('generic', 'Controller', 'Back', 'Start'),
};

export const MODEL_CHOICES: { id: ControllerModelId; name: string }[] =
  (Object.keys(CONTROLLER_MODELS) as ControllerModelId[]).map(id => ({ id, name: CONTROLLER_MODELS[id].name }));
