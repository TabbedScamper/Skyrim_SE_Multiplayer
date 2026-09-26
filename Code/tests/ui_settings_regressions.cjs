const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { createRequire } = require('node:module');

const uiRoot = path.resolve(__dirname, '../skyrim_ui');
const uiRequire = createRequire(path.join(uiRoot, 'package.json'));
const ts = uiRequire('typescript');
const source = fs.readFileSync(path.join(uiRoot, 'src/app/services/setting.service.ts'), 'utf8');
const compiled = ts.transpileModule(source, {
  compilerOptions: {
    module: ts.ModuleKind.CommonJS,
    target: ts.ScriptTarget.ES2020,
    experimentalDecorators: true,
  },
}).outputText;
const exportsObject = {};
vm.runInNewContext(compiled, {
  exports: exportsObject,
  require: name => name === '@angular/core'
    ? { Injectable: () => value => value }
    : uiRequire(name),
});

const stored = new Map([
  ['audio_volume', 0.2],
  ['audio_muted', true],
  ['font_size', 'xs'],
  ['language', 'fr'],
  ['party_anchor', 2],
  ['party_autoHideTime', 5],
]);
const store = {
  get: (key, fallback) => stored.has(key) ? stored.get(key) : fallback,
  getFloat: (key, fallback) => stored.has(key) ? Number(stored.get(key)) : fallback,
  getBool: (key, fallback) => stored.has(key) ? Boolean(stored.get(key)) : fallback,
  set: (key, value) => stored.set(key, value),
};
let activeLanguage;
const service = new exportsObject.SettingService(store, {
  getAvailableLangs: () => [{ id: 'en' }, { id: 'fr' }],
  getDefaultLang: () => 'en',
  setActiveLang: value => { activeLanguage = value; },
});

for (const [setting, key, initial, factory] of [
  ['volume', 'audio_volume', 0.2, 0.5],
  ['muted', 'audio_muted', true, false],
  ['fontSize', 'font_size', 'xs', 'm'],
  ['language', 'language', 'fr', 'en'],
  ['partyAnchor', 'party_anchor', 2, 0],
  ['autoHideTime', 'party_autoHideTime', 5, 1],
]) {
  const value = service.settings[setting];
  assert.equal(value.value, initial, setting);
  assert.equal(value.defaultValue, factory, setting);
  value.reset();
  assert.equal(value.value, factory, setting);
  assert.equal(stored.get(key), factory, setting);
}
assert.equal(activeLanguage, 'en');
service.settings.fontSize.next('invalid');
assert.equal(service.settings.fontSize.value, 'm');
console.log('Passed: persisted settings load, factory resets persist, and select validation remains intact.');
