import { Injectable, NgZone } from '@angular/core';
import { BehaviorSubject } from 'rxjs';
import { ClientService } from './client.service';
import { Sound, SoundService } from './sound.service';

/** One entry of the button prompt bar. `button` is a controller action name (see ClientService.gamepadInput) or 'lr'. */
export interface NavPrompt {
  button: string;
  label: string;
}

type Direction = 'up' | 'down' | 'left' | 'right';

/**
 * Controller navigation for every overlay menu.
 *
 * Menus opt in with attributes instead of per-component key handling:
 * - `data-nav-scope` marks a navigable surface; the topmost visible one owns input
 *   (`data-nav-scope="modal"` wins over the others).
 * - `data-nav-tabs` holds `[role=tab]` buttons: LB/RB switch them, and B or
 *   left-from-the-edge returns to the active tab from `data-nav-content`.
 * - `data-nav-default` is focused when the scope appears.
 * - `data-nav-x|y|start|view="Label"` binds that button to a click on the element.
 * - `data-nav-a="Label"` renames the A prompt for an element.
 *
 * Movement is spatial (nearest control in the pressed direction), sliders and
 * dropdowns change with left/right, B steps back one level, and the right
 * stick scrolls. Arrow keys on a keyboard use the same movement.
 */
@Injectable({ providedIn: 'root' })
export class GamepadNavigationService {
  /** True while the last input came from a controller or the arrow keys; drives focus styling and prompts. */
  readonly active$ = new BehaviorSubject<boolean>(false);
  readonly prompts$ = new BehaviorSubject<NavPrompt[]>([]);
  /** Controller model id from ControlBindings (glyph set for the prompts). */
  readonly model$ = new BehaviorSubject<string>('xbox-series');

  private static readonly focusable =
    'button:not([disabled]), input:not([disabled]):not([type="hidden"]), app-dropdown:not(.disabled), summary, [data-nav-item]';

  private rowElement?: Element;
  private scheduled = false;
  private modelRequested = false;

  constructor(
    private readonly client: ClientService,
    private readonly sound: SoundService,
    private readonly zone: NgZone,
  ) {
    client.gamepadInput.subscribe(({ action, repeat }) => this.onAction(action, repeat));
    client.gamepadScroll.subscribe(amount => this.scroll(amount));
    client.controlBindingsChange.subscribe(state => state.controller && this.model$.next(state.controller));

    this.zone.runOutsideAngular(() => {
      document.addEventListener('mousemove', event => {
        if (this.active$.value && Math.abs(event.movementX) + Math.abs(event.movementY) > 3)
          this.zone.run(() => this.setActive(false));
      });
      document.addEventListener('keydown', event => this.onKeyboard(event));
      document.addEventListener('focusin', () => this.schedule());
      document.addEventListener('focusout', () => this.schedule());
      new MutationObserver(() => this.schedule()).observe(document.body, {
        childList: true,
        subtree: true,
        attributes: true,
        attributeFilter: ['aria-expanded', 'open', 'disabled', 'aria-selected'],
      });
    });
  }

  // ---- input ----

  private onAction(action: string, repeat: boolean): void {
    const wasActive = this.active$.value;
    this.setActive(true);
    if (!this.modelRequested) {
      this.modelRequested = true;
      this.client.requestControlBindings();
    }

    const scope = this.scope();
    if (!scope) {
      this.legacyKey(action);
      return;
    }
    const current = this.current(scope);
    if (!current) {
      // First press after the menu appeared or after using the mouse: land somewhere sensible.
      if (!['b', 'lb', 'rb', 'lt', 'rt'].includes(action)) {
        this.focus(this.defaultTarget(scope), !wasActive);
        return;
      }
    }

    switch (action) {
      case 'up':
      case 'down':
      case 'left':
      case 'right':
        this.direction(scope, current, action, repeat);
        break;
      case 'a':
        this.accept(current);
        break;
      case 'b':
        this.back(scope, current);
        break;
      case 'lb':
      case 'rb':
        this.switchTab(scope, current, action === 'rb' ? 1 : -1);
        break;
      case 'lt':
      case 'rt':
        this.page(scope, current, action === 'rt' ? 1 : -1);
        break;
      case 'x':
      case 'y':
      case 'start':
      case 'view':
        this.bound(scope, action);
        break;
    }
    this.schedule();
  }

  /** Arrow keys (not inside text fields) use the same spatial movement. */
  private onKeyboard(event: KeyboardEvent): void {
    if (!event.isTrusted || event.defaultPrevented || event.altKey || event.ctrlKey || event.metaKey) return;
    const direction = ({ ArrowUp: 'up', ArrowDown: 'down', ArrowLeft: 'left', ArrowRight: 'right' } as Record<string, Direction>)[event.key];
    if (!direction) return;
    const target = event.target as HTMLElement;
    if (this.isTextField(target)) return;
    // Native range stepping stays with left/right on a keyboard.
    if (target instanceof HTMLInputElement && target.type === 'range' && (direction === 'left' || direction === 'right')) return;
    const scope = this.scope();
    if (!scope) return;
    event.preventDefault();
    this.zone.run(() => {
      this.setActive(true);
      const current = this.current(scope);
      if (!current) this.focus(this.defaultTarget(scope), false);
      else this.move(scope, current, direction);
    });
  }

  private direction(scope: HTMLElement, current: HTMLElement | null, direction: Direction, repeat: boolean): void {
    if (!current) return;
    const horizontal = direction === 'left' || direction === 'right';

    if (current.tagName === 'APP-DROPDOWN') {
      const open = current.getAttribute('aria-expanded') === 'true';
      // Open list: up/down walk it. Closed: left/right cycle the value, one step per press
      // (a held stick would otherwise apply every resolution on the way).
      if ((open && !horizontal) || (!open && horizontal && !repeat)) {
        this.key(current, direction === 'up' ? 'ArrowUp' : direction === 'down' ? 'ArrowDown' : direction === 'left' ? 'ArrowLeft' : 'ArrowRight');
        return;
      }
      if (open || horizontal) return;
    }

    if (horizontal && current instanceof HTMLInputElement && current.type === 'range') {
      this.stepRange(current, direction === 'right' ? 1 : -1);
      return;
    }

    this.move(scope, current, direction);
  }

  private accept(current: HTMLElement | null): void {
    if (!current) return;
    const scope = this.scope();
    if (scope && current.getAttribute('role') === 'tab' && this.tabs(scope).includes(current)) {
      if (current.getAttribute('aria-selected') !== 'true') current.click();
      setTimeout(() => this.enterContent(scope, current));
      return;
    }
    if (current.tagName === 'APP-DROPDOWN') {
      this.key(current, ' ');
    } else if (current instanceof HTMLInputElement && current.type === 'range') {
      current.dispatchEvent(new CustomEvent('gamepadadjust', { bubbles: true }));
    } else if (this.isTextField(current)) {
      current.focus();
      current.select?.();
    } else {
      current.click();
    }
  }

  private back(scope: HTMLElement, current: HTMLElement | null): void {
    if (current?.tagName === 'APP-DROPDOWN' && current.getAttribute('aria-expanded') === 'true') {
      this.key(current, 'Escape');
      return;
    }
    const tab = this.activeTab(scope);
    if (tab && current && !this.tabs(scope).includes(current)) {
      this.focus(tab, true, Sound.Cancel);
      return;
    }
    // Leave the menu through its own Escape handling (reverts a pending display change, closes).
    this.key((current ?? document.body) as HTMLElement, 'Escape');
  }

  private switchTab(scope: HTMLElement, current: HTMLElement | null, step: number): void {
    const tabs = this.tabs(scope);
    if (tabs.length < 2) return;
    const active = this.activeTab(scope);
    const index = active ? tabs.indexOf(active) : 0;
    const next = tabs[(index + step + tabs.length) % tabs.length];
    const onTab = !!current && tabs.includes(current);
    next.click();
    // Let the new section render, then keep focus where the player was working.
    setTimeout(() => {
      if (onTab || !current) this.focus(next, false);
      else {
        const content = scope.querySelector<HTMLElement>('[data-nav-content]');
        const first = content ? this.focusables(content)[0] : undefined;
        this.focus(first ?? next, false);
      }
    });
  }

  /** LT/RT: jump roughly a page through the section. */
  private page(scope: HTMLElement, current: HTMLElement | null, step: number): void {
    const container = this.scrollContainer(scope, current);
    if (!container) return;
    container.scrollBy({ top: step * container.clientHeight * 0.8, behavior: 'smooth' });
    setTimeout(() => {
      const box = container.getBoundingClientRect();
      const visible = this.focusables(container).filter(e => {
        const r = e.getBoundingClientRect();
        return r.top >= box.top && r.bottom <= box.bottom;
      });
      const target = step > 0 ? visible[visible.length - 1] : visible[0];
      if (target) this.focus(target, true);
    }, 250);
  }

  private bound(scope: HTMLElement, action: string): void {
    const target = Array.from(scope.querySelectorAll<HTMLElement>(`[data-nav-${action}]`)).find(e => this.visible(e) && !(e as HTMLButtonElement).disabled);
    if (!target) return;
    target.click();
  }

  private scroll(amount: number): void {
    const scope = this.scope();
    if (!scope) return;
    this.setActive(true);
    this.scrollContainer(scope, this.current(scope))?.scrollBy({ top: amount });
  }

  /** Outside any navigable surface (in-game chat, legacy popups) keep the old key behaviour. */
  private legacyKey(action: string): void {
    const keys: Record<string, string> = { up: 'ArrowUp', down: 'ArrowDown', left: 'ArrowLeft', right: 'ArrowRight', a: 'Enter', b: 'Escape' };
    const key = keys[action];
    if (!key) return;
    const target = (document.activeElement as HTMLElement) ?? document.body;
    if (action === 'a' && target !== document.body && !this.isTextField(target)) target.click();
    else this.key(target, key);
  }

  // ---- movement ----

  private move(scope: HTMLElement, current: HTMLElement, direction: Direction): void {
    const tabs = this.tabs(scope);
    if (tabs.includes(current) && this.tabsAreVertical(tabs) && direction === 'right') {
      this.enterContent(scope, current);
      return;
    }
    // The tab list is a column of its own: moves never wander into another tab from the content
    // (left off the edge goes back to the active one, below) or out of the list into the content.
    const onTab = tabs.includes(current);
    const items = this.focusables(scope).filter(e => e !== current && (tabs.length < 2 || tabs.includes(e) === onTab));
    // Inside a scrolling section, its own controls (even ones scrolled out of view) come
    // before anything outside it, such as the footer buttons under the panel.
    const content = current.closest('[data-nav-content]');
    const inside = content ? items.filter(e => content.contains(e)) : [];
    const best = (inside.length ? this.nearest(current, inside, direction) : undefined) ?? this.nearest(current, items, direction);
    if (best) this.focus(best, true);
    else if (direction === 'left' && content) this.focus(this.activeTab(scope), true);
  }

  private nearest(current: HTMLElement, items: HTMLElement[], direction: Direction): HTMLElement | undefined {
    const vertical = direction === 'down' || direction === 'up';
    const from = this.navRect(current);
    const fx = from.left + from.width / 2;
    const fy = from.top + from.height / 2;
    // Best candidate in the current row/column ("beam") and best one outside it.
    let beam: { item: HTMLElement; edge: number; score: number } | undefined;
    let other: { item: HTMLElement; edge: number; score: number } | undefined;
    for (const item of items) {
      const to = this.navRect(item);
      const tx = to.left + to.width / 2;
      const ty = to.top + to.height / 2;
      // Only controls actually past the current one count (a little overlap is allowed),
      // so a wide control above-left is not "left" of a button on the row below it.
      const slack = Math.min(from.width, from.height, to.width, to.height) * 0.25;
      const edge = direction === 'down' ? to.top - from.bottom
        : direction === 'up' ? from.top - to.bottom
        : direction === 'right' ? to.left - from.right
        : from.left - to.right;
      const pastCentre = direction === 'down' ? ty > fy : direction === 'up' ? ty < fy : direction === 'right' ? tx > fx : tx < fx;
      if (edge < -slack || !pastCentre) continue;
      const gap = vertical ? Math.max(0, to.left - from.right, from.left - to.right) : Math.max(0, to.top - from.bottom, from.top - to.bottom);
      const primary = vertical ? Math.abs(ty - fy) : Math.abs(tx - fx);
      const candidate = { item, edge: Math.max(0, edge), score: primary + gap * 4 };
      if (gap === 0) {
        if (!beam || candidate.edge < beam.edge || (candidate.edge === beam.edge && candidate.score < beam.score)) beam = candidate;
      } else if (!other || candidate.score < other.score) {
        other = candidate;
      }
    }
    // Left/right stay on the row whenever the row has something. Up/down stay in the column
    // unless something beside it is clearly nearer (a dropdown off to the side of a centred button).
    if (other && (!beam || (vertical && other.edge + 8 < beam.edge))) return other.item;
    return beam?.item;
  }

  /** Checkboxes and sliders are navigated as their whole row: the label is what the player looks at. */
  private navRect(e: HTMLElement): DOMRect {
    if (e instanceof HTMLInputElement && (e.type === 'checkbox' || e.type === 'radio' || e.type === 'range')) {
      const row = e.closest('.row, .option, label');
      if (row && row.querySelectorAll('input, button, app-dropdown').length === 1) return row.getBoundingClientRect();
    }
    return e.getBoundingClientRect();
  }

  private tabsAreVertical(tabs: HTMLElement[]): boolean {
    return tabs.length > 1 && Math.abs(tabs[1].getBoundingClientRect().top - tabs[0].getBoundingClientRect().top) > 4;
  }

  /** From a tab into its section: the first control of the content. */
  private enterContent(scope: HTMLElement, tab: HTMLElement): void {
    const content = scope.querySelector<HTMLElement>('[data-nav-content]');
    const first = content ? this.focusables(content)[0] : undefined;
    if (first) this.focus(first, true);
    else this.sound.play(Sound.Fail);
  }

  private stepRange(input: HTMLInputElement, direction: number): void {
    const step = parseFloat(input.step) || 1;
    const min = input.min === '' ? 0 : parseFloat(input.min);
    const max = input.max === '' ? 100 : parseFloat(input.max);
    const decimals = (input.step.split('.')[1] ?? '').length;
    const value = Math.min(max, Math.max(min, parseFloat(input.value) + direction * step));
    const text = value.toFixed(decimals);
    if (text === input.value) return;
    input.value = text;
    input.dispatchEvent(new Event('input', { bubbles: true }));
    input.dispatchEvent(new Event('change', { bubbles: true }));
    input.dispatchEvent(new CustomEvent('gamepadadjust', { bubbles: true }));
    this.sound.play(Sound.Focus);
  }

  private focus(target: HTMLElement | undefined, withSound: boolean, sound: Sound = Sound.Focus): void {
    if (!target) return;
    // Moving along the tab list shows each section, as in Skyrim's own menus.
    if (target.getAttribute('role') === 'tab' && target.getAttribute('aria-selected') !== 'true' && target.closest('[data-nav-tabs]')) {
      target.click();
      withSound = false; // the tab plays its own focus sound
    }
    target.focus({ preventScroll: true });
    target.scrollIntoView({ behavior: 'smooth', block: 'nearest', inline: 'nearest' });
    if (withSound) this.sound.play(sound);
    this.schedule();
  }

  // ---- queries ----

  scope(): HTMLElement | null {
    const scopes = Array.from(document.querySelectorAll<HTMLElement>('[data-nav-scope]')).filter(e => this.visible(e));
    return scopes.filter(e => e.dataset['navScope'] === 'modal').pop() ?? scopes.pop() ?? null;
  }

  private current(scope: HTMLElement): HTMLElement | null {
    const active = document.activeElement as HTMLElement | null;
    return active && active !== document.body && scope.contains(active) && this.visible(active) ? active : null;
  }

  private focusables(root: HTMLElement): HTMLElement[] {
    return Array.from(root.querySelectorAll<HTMLElement>(GamepadNavigationService.focusable)).filter(
      e => this.visible(e) && !e.closest('[data-nav-skip]') && !e.closest('app-dropdown ul') && (e.tagName === 'APP-DROPDOWN' || !e.closest('app-dropdown')),
    );
  }

  private tabs(scope: HTMLElement): HTMLElement[] {
    const holder = scope.querySelector('[data-nav-tabs]');
    return holder ? Array.from(holder.querySelectorAll<HTMLElement>('[role="tab"]')).filter(e => this.visible(e)) : [];
  }

  private activeTab(scope: HTMLElement): HTMLElement | undefined {
    const tabs = this.tabs(scope);
    return tabs.find(t => t.getAttribute('aria-selected') === 'true') ?? tabs[0];
  }

  private defaultTarget(scope: HTMLElement): HTMLElement | undefined {
    const preferred = Array.from(scope.querySelectorAll<HTMLElement>('[data-nav-default]')).find(e => this.visible(e));
    return preferred ?? this.activeTab(scope) ?? this.focusables(scope)[0];
  }

  private scrollContainer(scope: HTMLElement, from: HTMLElement | null): HTMLElement | null {
    for (let e: HTMLElement | null = from; e && scope.contains(e); e = e.parentElement) {
      const style = getComputedStyle(e);
      if (/(auto|scroll)/.test(style.overflowY) && e.scrollHeight > e.clientHeight + 1) return e;
    }
    const content = scope.querySelector<HTMLElement>('[data-nav-content]');
    if (content && content.scrollHeight > content.clientHeight + 1) return content;
    return Array.from(scope.querySelectorAll<HTMLElement>('*')).find(e => {
      const style = getComputedStyle(e);
      return /(auto|scroll)/.test(style.overflowY) && e.scrollHeight > e.clientHeight + 1;
    }) ?? null;
  }

  private visible(e: Element): boolean {
    // A collapsed <details> still reports a layout box for its content, which cannot take focus.
    for (let d = e.closest('details'); d; d = d.parentElement?.closest('details') ?? null)
      if (!d.open && !(e.tagName === 'SUMMARY' && e.parentElement === d)) return false;
    const check = (e as Element & { checkVisibility?: (o: object) => boolean }).checkVisibility;
    if (check && !check.call(e, { checkVisibilityCSS: true })) return false;
    const r = e.getBoundingClientRect();
    return r.width > 0 && r.height > 0 && getComputedStyle(e).visibility !== 'hidden';
  }

  private isTextField(e: Element | null): e is HTMLInputElement {
    return e instanceof HTMLTextAreaElement ||
      (e instanceof HTMLInputElement && !['button', 'checkbox', 'radio', 'range', 'submit', 'reset'].includes(e.type));
  }

  private key(target: HTMLElement, key: string): void {
    target.dispatchEvent(new KeyboardEvent('keydown', { key, bubbles: true, cancelable: true }));
  }

  // ---- state and prompts ----

  private setActive(active: boolean): void {
    if (this.active$.value === active) return;
    this.active$.next(active);
    document.body.classList.toggle('gamepad-mode', active);
    this.schedule();
  }

  private schedule(): void {
    if (this.scheduled) return;
    this.scheduled = true;
    requestAnimationFrame(() => {
      this.scheduled = false;
      this.zone.run(() => this.update());
    });
  }

  private update(): void {
    const scope = this.active$.value ? this.scope() : null;
    let current = scope ? this.current(scope) : null;
    // A menu just opened (or the focused control disappeared): pick its default.
    if (scope && !current) {
      const target = this.defaultTarget(scope);
      if (target) {
        target.focus({ preventScroll: true });
        current = this.current(scope);
      }
    }

    const row = current?.closest('.row, .binding, .option, .member, .friend, label') ?? current ?? undefined;
    if (row !== this.rowElement) {
      this.rowElement?.classList.remove('nav-row');
      row?.classList.add('nav-row');
      this.rowElement = row;
    }

    this.prompts$.next(scope && current ? this.promptsFor(scope, current) : []);
  }

  private promptsFor(scope: HTMLElement, current: HTMLElement): NavPrompt[] {
    const prompts: NavPrompt[] = [];
    const isDropdown = current.tagName === 'APP-DROPDOWN';
    const open = isDropdown && current.getAttribute('aria-expanded') === 'true';
    const isRange = current instanceof HTMLInputElement && current.type === 'range';

    let accept = current.getAttribute('data-nav-a');
    if (!accept) {
      if (isDropdown) accept = open ? 'Choose' : 'Open';
      else if (current instanceof HTMLInputElement && current.type === 'checkbox') accept = current.checked ? 'Turn off' : 'Turn on';
      else if (this.isTextField(current)) accept = 'Type';
      else if (current.tagName === 'SUMMARY') accept = (current.parentElement as HTMLDetailsElement).open ? 'Hide' : 'Show';
      else if (!isRange) accept = 'Select';
    }
    if (accept) prompts.push({ button: 'a', label: accept });
    if (isRange || (isDropdown && !open)) prompts.push({ button: 'lr', label: 'Adjust' });

    const tab = this.activeTab(scope);
    const back = open ? 'Cancel' : tab && !this.tabs(scope).includes(current) ? 'Back' : 'Close';
    prompts.push({ button: 'b', label: back });

    for (const action of ['x', 'y', 'start', 'view']) {
      const bound = Array.from(scope.querySelectorAll<HTMLElement>(`[data-nav-${action}]`)).find(e => this.visible(e) && !(e as HTMLButtonElement).disabled);
      const label = bound?.getAttribute(`data-nav-${action}`);
      if (label) prompts.push({ button: action, label });
    }
    if (this.tabs(scope).length > 1) prompts.push({ button: 'lbrb', label: 'Section' });
    if (this.scrollContainer(scope, current)) prompts.push({ button: 'rs', label: 'Scroll' });
    return prompts;
  }
}
