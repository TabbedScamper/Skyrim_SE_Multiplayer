const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { createRequire } = require('node:module');

// Research (docs/REFERENCE_RESEARCH.md is outside this task's write allowlist):
// https://github.com/schlangster/skyui/blob/master/src/ModConfigPanel/MenuDialog.as
// initContent/handleInput/onMenuListPress keep list focus and consume dialog input.
// Adopt a custom list with separate highlighted and committed choices; use explicit
// A-confirm/B-cancel instead of SkyUI's exit-accept convention or a native CEF popup.
// Local OverlayService.cpp:PollControllerNavigation folds both stick axes into D-pad
// bits, then sends one event per axis with a 400ms/110ms repeat. There are no raw
// axes or release events in JS. True deadzone/hysteresis must be fixed upstream,
// not inferred here. These tests cover the UI's burst/repeat and geometry guards.
// No native probes or offsets are introduced. This is not an in-game validation.

const uiRoot = path.resolve(__dirname, '../skyrim_ui');
const uiRequire = createRequire(path.join(uiRoot, 'package.json'));
const ts = uiRequire('typescript');
const { Subject } = uiRequire('rxjs');
let now = 1000;
const rect = (left, top, width = 100, height = 30) =>
  ({ left, top, width, height, right: left + width, bottom: top + height });

class Element {
  constructor(tag = 'button', box = rect(0, 0), parent = null) {
    this.tagName = tag.toUpperCase();
    this.box = box;
    this.parentElement = parent;
    this.attributes = {};
    this.dataset = {};
    this.style = { visibility: 'visible', overflowY: 'visible' };
    this.classes = new Set();
    this.classList = {
      add: name => this.classes.add(name), remove: name => this.classes.delete(name),
      toggle: (name, enabled) => enabled ? this.classes.add(name) : this.classes.delete(name),
    };
    this.clientTop = 0;
    this.clientHeight = box.height;
    this.scrollHeight = box.height;
    this.scrollTop = 0;
    this.scrollLeft = 0;
    this.clicks = 0;
  }
  getAttribute(name) { return this.attributes[name] ?? null; }
  getBoundingClientRect() { return this.box; }
  contains(node) {
    for (; node; node = node.parentElement) if (node === this) return true;
    return false;
  }
  matches(selector) {
    return selector.includes(':disabled') && (this.disabled || this.attributes['aria-disabled'] === 'true');
  }
  closest(selector) {
    for (let node = this; node; node = node.parentElement) {
      if (selector === '[data-nav-column]' && node.column) return node;
      if (selector === '[data-nav-content]' && node.content) return node;
    }
    return null;
  }
  focus() { document.activeElement = this; }
  click() { this.clicks++; }
  querySelector() { return null; }
  querySelectorAll() { return []; }
  dispatchEvent(event) { this.onKey?.(event); return !event.defaultPrevented; }
}
class Input extends Element {}
class Textarea extends Element {}
class KeyEvent {
  constructor(type, options) { Object.assign(this, { type, defaultPrevented: false, repeat: false, isTrusted: false }, options); }
  preventDefault() { this.defaultPrevented = true; }
  stopPropagation() { this.stopped = true; }
}
const body = new Element('body');
const document = { body, activeElement: body, addEventListener() {}, contains: node => body.contains(node) };
const zone = { run: fn => fn(), runOutsideAngular: fn => fn() };
const decorator = () => () => {};
const core = {
  Injectable: decorator, Component: decorator, HostBinding: decorator, HostListener: decorator,
  ContentChildren: decorator, Input: decorator, Output: decorator, forwardRef: fn => fn,
  EventEmitter: Subject,
};
const sound = { play() {} };
function load(relativePath, exportedName) {
  const compiled = ts.transpileModule(fs.readFileSync(path.join(uiRoot, 'src/app', relativePath), 'utf8'), {
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020, experimentalDecorators: true },
  }).outputText;
  const exports = {};
  vm.runInNewContext(compiled, {
    exports, document, performance: { now: () => now },
    HTMLInputElement: Input, HTMLTextAreaElement: Textarea, KeyboardEvent: KeyEvent,
    Event: KeyEvent, CustomEvent: KeyEvent, getComputedStyle: e => e.style,
    MutationObserver: class { observe() {} }, requestAnimationFrame() {}, setTimeout: fn => fn(),
    require: name => name === '@angular/core' ? core : name === '@angular/forms' ? {} :
      name.includes('sound.service') ? { Sound: {} } : name === 'rxjs' ? uiRequire(name) : {},
  });
  return exports[exportedName];
}
const Navigation = load('services/gamepad-navigation.service.ts', 'GamepadNavigationService');
const Dropdown = load('components/dropdown/dropdown.component.ts', 'DropdownComponent');
function navigation() {
  const client = { gamepadInput: new Subject(), gamepadScroll: new Subject(), controlBindingsChange: new Subject(), requestControlBindings() {} };
  const nav = new Navigation(client, sound, zone);
  const scope = new Element('section', rect(0, 0, 600, 500), body);
  let items = [];
  nav.scope = () => scope;
  nav.focusables = () => items;
  nav.tabs = () => [];
  nav.promptsFor = () => [];
  nav.schedule = () => {};
  return { nav, scope, client, items: values => { items = values; } };
}
function dropdown() {
  const host = new Element('app-dropdown');
  const component = new Dropdown(new Subject(), sound, { detectChanges() {} }, { nativeElement: host });
  component.options.next(['Novice', 'Apprentice', 'Adept', 'Expert', 'Master', 'Legendary'].map((text, value) => ({ text, value })));
  const changes = [];
  let touched = 0;
  component.registerOnChange(value => changes.push(value));
  component.registerOnTouched(() => touched++);
  component.writeValue(2);
  component.optSelect.emit = () => {};
  host.onKey = event => component.keydownHandler(event);
  host.getAttribute = name => name === 'aria-expanded' ? component.expanded : name === 'aria-disabled' ? component.ariaDisabled : null;
  return { component, host, changes, touched: () => touched };
}
function key(component, value, repeat = false) {
  const event = new KeyEvent('keydown', { key: value, repeat });
  component.keydownHandler(event);
  return event;
}

// Reproduce a poll's down+right pair and jitter reported as additional initial presses.
{
  const { nav, scope, client, items } = navigation();
  const buttons = [0, 1, 2, 3].map(i => new Element('button', rect(0, i * 40), scope));
  items(buttons);
  buttons[0].focus();
  client.gamepadInput.next({ action: 'down', repeat: false });
  assert.equal(document.activeElement, buttons[1]);
  client.gamepadInput.next({ action: 'right', repeat: false });
  now += 20;
  client.gamepadInput.next({ action: 'down', repeat: false });
  assert.equal(document.activeElement, buttons[1], 'one move per native input burst');
  now += 100;
  client.gamepadInput.next({ action: 'down', repeat: true });
  assert.equal(document.activeElement, buttons[1], 'no premature repeat');
  now += 280;
  client.gamepadInput.next({ action: 'down', repeat: true });
  assert.equal(document.activeElement, buttons[2]);
  client.gamepadInput.next({ action: 'down', repeat: true });
  assert.equal(document.activeElement, buttons[2], 'duplicate repeat is ignored');
  now += 110;
  client.gamepadInput.next({ action: 'down', repeat: true });
  assert.equal(document.activeElement, buttons[3], 'normal native cadence still repeats');
  client.gamepadInput.next({ action: 'a', repeat: true });
  assert.equal(buttons[3].clicks, 0, 'held accept never reactivates a control');
  nav.scope = () => new Element('section');
  now += 110;
  client.gamepadInput.next({ action: 'down', repeat: true });
  assert.equal(document.activeElement, buttons[3], 'held direction cannot leak into another scope');
}

// Down must not zigzag into a closer row in the other column; right still crosses.
{
  const { nav, scope, items } = navigation();
  const party = new Element('section', rect(0, 0, 200, 400), scope); party.column = true;
  const campaign = new Element('section', rect(250, 0, 200, 400), scope); campaign.column = true;
  const first = new Element('button', rect(0, 0), party);
  const below = new Element('button', rect(0, 120), party);
  const across = new Element('button', rect(250, 40), campaign);
  const footer = new Element('button', rect(0, 450), scope);
  items([first, below, across, footer]);
  first.focus();
  nav.move(scope, first, 'down');
  assert.equal(document.activeElement, below);
  nav.move(scope, first, 'right');
  assert.equal(document.activeElement, across);
  nav.move(scope, below, 'down');
  assert.equal(document.activeElement, footer);
  first.focus();
  nav.active$.next(true);
  nav.update();
  across.box = rect(0, 0); // A reflow puts another column at the old coordinates.
  document.activeElement = body;
  items([below, across, footer]);
  nav.update();
  assert.equal(document.activeElement, below, 'removed focus recovers within its original column');
  nav.update();
  assert.equal(document.activeElement, below, 'unrelated renders preserve live focus');
  below.disabled = true;
  assert.equal(nav.current(scope), null, 'disabled controls cannot retain navigation ownership');
}

// Stable tie order, no backward/overlapping candidate, and no horizontal panel scrolling.
{
  const { nav, scope } = navigation();
  const first = new Element('button', rect(0, 0), scope);
  const right = new Element('button', rect(110, 0), scope);
  const tied = new Element('button', rect(110, 0), scope);
  const overlap = new Element('button', rect(10, 0), scope);
  assert.equal(nav.nearest(first, [overlap, right, tied], 'right'), right);
  assert.equal(nav.nearest(right, [first, overlap], 'right'), undefined);
  scope.style.overflowY = 'auto';
  scope.clientHeight = 100;
  scope.scrollHeight = 500;
  scope.scrollLeft = 17;
  first.box = rect(700, 200);
  nav.focus(first, false);
  assert.equal(scope.scrollTop, 130);
  assert.equal(scope.scrollLeft, 17);
}

// Controller routing exercises the actual shared dropdown handler and value accessor.
{
  const { nav, scope, items } = navigation();
  const { component, host, changes } = dropdown();
  host.parentElement = scope;
  items([host]);
  host.focus();
  nav.accept(host);
  assert.equal(component.isOpen, true);
  nav.direction(scope, host, 'down', false);
  assert.equal(component.highlighted, 3);
  assert.equal(component.selectedLabel, 'Adept', 'highlighting does not change the committed label');
  assert.deepEqual(changes, []);
  nav.back(scope, host);
  assert.equal(component.isOpen, false);
  assert.equal(component.selectedLabel, 'Adept');
  nav.accept(host);
  nav.direction(scope, host, 'down', false);
  nav.accept(host);
  assert.deepEqual(changes, [3]);
  assert.equal(component.selectedLabel, 'Expert');
  nav.direction(scope, host, 'right', false);
  nav.direction(scope, host, 'right', true);
  assert.deepEqual(changes, [3, 4], 'closed dropdown cycles once per press');
  nav.accept(host);
  nav.direction(scope, host, 'right', false);
  assert.equal(component.isOpen, true, 'sideways input cannot commit an open list');
}

// Keyboard/mouse use the same selection contract; blur/cancel do not emit changes.
{
  const { component, changes, touched } = dropdown();
  key(component, 'Enter');
  key(component, 'ArrowDown', true);
  key(component, 'Enter', true);
  assert.equal(component.isOpen, true, 'held Enter cannot immediately confirm');
  const escape = key(component, 'Escape');
  assert.equal(escape.stopped, true, 'B/Escape must not also close the owning menu');
  assert.equal(component.selectedLabel, 'Adept');
  assert.deepEqual(changes, []);
  component.toggle();
  component.optionSelect(5, 5); // mouse option click
  assert.deepEqual(changes, [5]);
  key(component, 'ArrowRight');
  key(component, 'ArrowRight', true);
  assert.deepEqual(changes, [5, 0], 'keyboard cycles wrap and do not repeat');
  component.toggle();
  key(component, 'ArrowDown');
  component.focusOutHandler();
  assert.equal(component.isOpen, false);
  assert.equal(component.selectedLabel, 'Novice');
  assert.ok(touched() > 0);
  component.setDisabledState(true);
  component.toggle();
  component.optionSelect(3, 3);
  key(component, 'ArrowRight');
  assert.deepEqual(changes, [5, 0], 'host-only rules reject every disabled input path');
  assert.equal(component.tabindex, -1);
  assert.equal(component.ariaDisabled, 'true');
  component.setDisabledState(false);
  component.options.next([]);
  component.toggle();
  key(component, 'ArrowRight');
  assert.equal(component.isOpen, false, 'empty lists do not open or throw');
}

// Document navigation ignores already consumed and synthetic keys.
{
  const { nav, scope, items } = navigation();
  const first = new Element('button', rect(0, 0), scope);
  const second = new Element('button', rect(0, 60), scope);
  items([first, second]); first.focus();
  nav.onKeyboard(new KeyEvent('keydown', { key: 'ArrowDown', target: first }));
  assert.equal(document.activeElement, first);
  nav.onKeyboard(new KeyEvent('keydown', { key: 'ArrowDown', target: first, isTrusted: true, defaultPrevented: true }));
  assert.equal(document.activeElement, first);
  nav.onKeyboard(new KeyEvent('keydown', { key: 'ArrowDown', target: first, isTrusted: true }));
  assert.equal(document.activeElement, second);
}

function templates(dir) {
  return fs.readdirSync(dir, { withFileTypes: true }).flatMap(entry => {
    const file = path.join(dir, entry.name);
    return entry.isDirectory() ? templates(file) : file.endsWith('.html') ? [file] : [];
  });
}
for (const file of templates(path.join(uiRoot, 'src/app')))
  assert.doesNotMatch(fs.readFileSync(file, 'utf8'), /<select\b/i, `${file}: use the pad-friendly shared dropdown`);
console.log('Passed: input bursts/repeats, column navigation, focus recovery, stable geometry, dropdown pad/keyboard/mouse/cancel/disabled behavior, and no native selects.');
