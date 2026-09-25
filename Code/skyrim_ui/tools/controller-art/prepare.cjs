// Copies the controller overlays from AL2009man's Gamepad Asset Pack (MIT) into
// src/assets/images/controllers/, trimmed for the Controls page: editor metadata
// and hidden layers removed, a few bright details toned down. The page shows
// them through a greyscale filter; button anchors live in controller-models.ts.
//
// Usage: node tools/controller-art/prepare.cjs <Gamepad-Asset-Pack checkout>
// Needs Playwright's Chromium (set PLAYWRIGHT_CHROMIUM to its chrome.exe if it
// is not the bundled one).
const fs = require('fs');
const path = require('path');
const { pathToFileURL } = require('url');
const { chromium } = require('playwright');

const SOURCES = {
  'xbox-series': 'Xbox Wireless Controller Images/Default Theme/Theme SVG/Xbox Series X Color/Xbox Series X Controller VSCView Black.svg',
  'xbox-one': 'Xbox Wireless Controller Images/Default Theme/Theme SVG/Xbox One Color/Xbox One Controller VSCView Black.svg',
  'xbox-360': 'Xbox 360 Controller Images/Default Theme/Theme SVG/Xbox 360 VSCView - Black.svg',
  dualsense: 'DualSense Controller Image/Default/Theme SVG/DualSense VSCView SVG Midnight Black.svg',
  dualshock4: 'DualShock 4 Controller Images/Default Theme/Theme SVG/DS4 V2 VSC SVG.svg',
  'switch-pro': 'Nintendo Switch Controller Images/Switch Pro Controller/Default Theme/Theme SVG/Switch Pro Controller VSCView.svg',
  'steam-deck': 'Steam Deck Images/Theme SVG/Black/Steam Deck VSCView Overlay.svg',
  'steam-controller': 'Steam Controller Images/Default Theme/Theme SVG/Black/Steam Controller VSCView.svg',
};

// Elements dropped by inkscape:label (per model).
const DROP = {
  'xbox-360': ['Xbox 360 Text'], // battery pack lettering, mirrored in a front view
};

// Colour swaps applied to the serialised file.
const RECOLOR = {
  'steam-deck': [['#19ff00', '#0c0c0c']], // chroma-key green screen -> an unlit panel
};

(async () => {
  const pack = process.argv[2];
  if (!pack) throw new Error('usage: prepare.cjs <Gamepad-Asset-Pack checkout>');
  const outDir = path.join(__dirname, '../../src/assets/images/controllers');
  fs.mkdirSync(outDir, { recursive: true });
  const browser = await chromium.launch({ executablePath: process.env.PLAYWRIGHT_CHROMIUM || undefined });
  const page = await browser.newPage();
  for (const [id, rel] of Object.entries(SOURCES)) {
    await page.goto(pathToFileURL(path.join(pack, 'Controller Asset Pack', rel)).href);
    let svg = await page.evaluate(drop => {
      const INK = 'http://www.inkscape.org/namespaces/inkscape';
      const root = document.documentElement;
      // Hidden layers first (computed, so inherited display:none counts).
      const hidden = [...root.querySelectorAll('g, path, rect, circle, ellipse, use, text, image')]
        .filter(e => !e.closest('defs, clipPath, mask, pattern, symbol') && getComputedStyle(e).display === 'none');
      hidden.forEach(e => e.remove());
      root.querySelectorAll('*').forEach(e => { if (drop.includes(e.getAttributeNS(INK, 'label'))) e.remove(); });
      root.querySelectorAll('metadata, title').forEach(e => e.remove());
      [...root.getElementsByTagName('sodipodi:namedview')].forEach(e => e.remove());
      root.querySelectorAll('*').forEach(e => {
        for (const a of [...e.attributes]) {
          if (/^(inkscape|sodipodi):/.test(a.name) || a.name === 'xml:space') e.removeAttribute(a.name);
        }
      });
      for (const a of [...root.attributes]) {
        if (/^(width|height|x|y|version)$/.test(a.name) || /^xmlns:(inkscape|sodipodi)$/.test(a.name)) root.removeAttribute(a.name);
      }
      return new XMLSerializer().serializeToString(root);
    }, DROP[id] || []);
    for (const [from, to] of RECOLOR[id] || []) svg = svg.split(from).join(to);
    svg = svg.replace(/>\s+</g, '><').replace(/-inkscape-[a-z-]+:[^;"]*;?/g, '');
    const note = `<!-- ${path.basename(rel)} from Gamepad Asset Pack by AL2009man (MIT). See NOTICE.md. -->`;
    fs.writeFileSync(path.join(outDir, `${id}.svg`), note + svg);
    console.log(id, (svg.length / 1024).toFixed(0) + ' KB');
  }
  await browser.close();
})();
