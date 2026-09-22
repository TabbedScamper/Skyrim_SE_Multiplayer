import fs from 'node:fs';

const action = process.argv[2] ?? 'snapshot';
const outputPath = process.argv[3];
const targets = await (await fetch('http://127.0.0.1:8384/json')).json();
const target = targets.find(item => item.type === 'page' && item.webSocketDebuggerUrl);
if (!target) throw new Error('CEF DevTools page target was not found');

const socket = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((resolve, reject) => {
  socket.addEventListener('open', resolve, { once: true });
  socket.addEventListener('error', reject, { once: true });
});

let nextId = 1;
const pending = new Map();
socket.addEventListener('message', event => {
  const message = JSON.parse(event.data);
  if (!message.id || !pending.has(message.id)) return;
  const { resolve, reject } = pending.get(message.id);
  pending.delete(message.id);
  if (message.error) reject(new Error(message.error.message));
  else resolve(message.result);
});

function call(method, params = {}) {
  const id = nextId++;
  return new Promise((resolve, reject) => {
    pending.set(id, { resolve, reject });
    socket.send(JSON.stringify({ id, method, params }));
  });
}

const snapshotExpression = `(() => {
  const active = document.activeElement;
  const dropdowns = [...document.querySelectorAll('app-dropdown')].map((host, index) => {
    const wrapper = host.querySelector('.dropdown-wrapper');
    const selected = host.querySelector('.dropdown-options .selected');
    return {
      index,
      id: host.id || null,
      focused: active === host,
      open: !!wrapper?.classList.contains('is-open'),
      selectedText: selected?.textContent?.trim() ?? null,
      selectedIndex: [...host.querySelectorAll('.dropdown-options li')].indexOf(selected),
      label: host.querySelector('.dropdown-selected')?.textContent?.trim() ?? null
    };
  });
  const inputs = [...document.querySelectorAll('input')].map(input => ({
    id: input.id || null,
    context: input.closest('app-settings') ? 'settings' : input.closest('.coop-lobby') ? 'coop-lobby' : 'other',
    type: input.type,
    value: input.value,
    checked: input.checked,
    focused: active === input
  }));
  return {
    url: location.href,
    title: document.title,
    settingsVisible: !!document.querySelector('app-settings'),
    active: active ? { tag: active.tagName, id: active.id || null, text: active.textContent?.trim() || null } : null,
    dropdowns,
    inputs,
    bodySize: { width: document.body.clientWidth, height: document.body.clientHeight }
  };
})()`;

let result;
if (action === 'snapshot') {
  const evaluated = await call('Runtime.evaluate', {
    expression: snapshotExpression,
    returnByValue: true,
    awaitPromise: true
  });
  result = evaluated.result.value;
} else if (action === 'focus-display-mode') {
  const evaluated = await call('Runtime.evaluate', {
    expression: `(() => { const e = document.querySelector('app-dropdown#displayMode'); e?.focus(); return !!e; })()`,
    returnByValue: true
  });
  result = { focused: evaluated.result.value };
} else if (action === 'screenshot') {
  if (!outputPath) throw new Error('screenshot requires an output path');
  await call('Page.enable');
  const capture = await call('Page.captureScreenshot', { format: 'png', fromSurface: true });
  fs.writeFileSync(outputPath, Buffer.from(capture.data, 'base64'));
  result = { path: outputPath };
} else {
  throw new Error(`Unknown CDP action: ${action}`);
}

console.log(JSON.stringify(result));
socket.close();
