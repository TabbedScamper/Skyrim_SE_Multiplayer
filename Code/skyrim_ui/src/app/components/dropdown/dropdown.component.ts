import {
  AfterViewInit,
  ChangeDetectorRef,
  Component,
  ContentChildren,
  ElementRef,
  EventEmitter,
  forwardRef,
  HostBinding,
  HostListener,
  Input,
  Output,
  QueryList,
} from '@angular/core';
import { ControlValueAccessor, NG_VALUE_ACCESSOR } from '@angular/forms';
import { BehaviorSubject, startWith, takeUntil } from 'rxjs';
import { Sound, SoundService } from 'src/app/services/sound.service';
import { DestroyService } from '../../services/destroy.service';
import { DropdownOptionComponent } from './dropdown-option.component';

type OptionValue = string | number;

const noop = () => {};

let dropdownCounter = 1;

@Component({
  selector: 'app-dropdown',
  templateUrl: './dropdown.component.html',
  styleUrls: ['./dropdown.component.scss'],
  providers: [
    {
      provide: NG_VALUE_ACCESSOR,
      useExisting: forwardRef(() => DropdownComponent),
      multi: true,
    },
    DestroyService,
  ],
})
export class DropdownComponent implements AfterViewInit, ControlValueAccessor {
  @HostBinding('attr.tabindex') get tabindex(): number { return this.isDisabled ? -1 : 0; }
  @HostBinding('attr.role') role = 'combobox';
  @HostBinding('attr.aria-haspopup') readonly hasPopup = 'listbox';
  @HostBinding('attr.aria-disabled') get ariaDisabled(): string { return String(this.isDisabled); }
  @HostBinding('attr.aria-controls') get controls(): string { return `list-${this.dropdownCounter}`; }
  @HostBinding('attr.aria-activedescendant') get activeDescendant(): string | null {
    return this.isOpen && this.highlighted >= 0 ? `li-${this.dropdownCounter}-${this.highlighted}` : null;
  }
  /** Read by GamepadNavigationService: an open list takes up/down, B closes it. */
  @HostBinding('attr.aria-expanded') get expanded(): string {
    return this.isOpen ? 'true' : 'false';
  }
  @HostBinding('class.disabled') get disabledClass(): boolean {
    return this.isDisabled;
  }
  dropdownCounter = dropdownCounter++;
  isOpen = false;
  isDisabled = false;
  selected: number | undefined = undefined;
  highlighted = -1;

  options = new BehaviorSubject<{ text: string; value: any }[]>([]);

  @ContentChildren(DropdownOptionComponent)
  optionChildren!: QueryList<DropdownOptionComponent>;
  @Input() placeholder: string;
  // translation is opt-out because it should be the default for all UI elements.
  @Input('noTranslate') noTranslate: boolean | undefined;
  @Output() optSelect = new EventEmitter();

  private onTouchedCallback: () => void = noop;
  private onChangeCallback: (_: any) => void = noop;
  private currentValue: any;

  key: string;
  isFocused: boolean;

  constructor(
    private readonly destroy$: DestroyService,
    private readonly soundService: SoundService,
    private readonly cdr: ChangeDetectorRef,
    private readonly elRef: ElementRef,
  ) {}

  ngAfterViewInit() {
    this.optionChildren.changes
      .pipe(takeUntil(this.destroy$), startWith(null))
      .subscribe(() => {
        const options = this.optionChildren.toArray();
        const mappedOptions = options.map(o => ({
            text: o.text,
            value: o.value,
          }));
        this.options.next(mappedOptions);
        if (this.currentValue != null)
          this.selected = mappedOptions.findIndex(o => o.value === this.currentValue);
        this.highlighted = this.selected ?? -1;
        this.cdr.detectChanges();
      });
  }

  @HostListener('focus')
  focusHandler() {
    this.isFocused = true;
  }

  @HostListener('focusout')
  focusOutHandler() {
    this.isFocused = false;
    this.isOpen = false;
    this.highlighted = this.selected ?? -1;
    this.onTouchedCallback();
  }

  @HostListener('keydown', ['$event'])
  keydownHandler(event: KeyboardEvent) {
    if (this.isDisabled) return;

    if (event.key === 'Escape' || event.key === 'Esc') {
      // Close only the list; the menu behind it stays open.
      if (!this.isOpen) return;
      this.toggle();
    } else if (event.key === ' ' || event.key === 'Enter') {
      if (event.repeat) {
        event.preventDefault();
        event.stopPropagation();
        return;
      }
      if (this.isOpen && this.highlighted >= 0) {
        const option = this.options.getValue()[this.highlighted];
        if (option) this.optionSelect(option.value, this.highlighted);
      } else {
        this.toggle();
      }
    } else if (event.key === 'ArrowUp' || event.key === 'ArrowDown') {
      if (!this.isOpen) return;
      const options = this.options.getValue();
      if (!options.length) return;
      const direction = event.key === 'ArrowDown' ? 1 : -1;
      const current = this.highlighted >= 0 ? this.highlighted : 0;
      this.highlighted = (current + direction + options.length) % options.length;
      this.revealOption();
    } else if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') {
      // An open list owns arrows; sideways movement must not commit its preview.
      if (!this.isOpen && !event.repeat) {
        const options = this.options.getValue();
        if (!options.length) return;
        const direction = event.key === 'ArrowRight' ? 1 : -1;
        const current = this.selected >= 0 ? this.selected : (direction > 0 ? -1 : 0);
        const next = (current + direction + options.length) % options.length;
        this.optionSelect(options[next].value, next);
      }
    } else {
      return;
    }

    event.preventDefault();
    event.stopPropagation();
  }

  optionSelect(selectedOption: any, idx: number) {
    if (this.isDisabled) return;
    this.currentValue = selectedOption;
    this.selected = idx;
    this.highlighted = idx;
    this.isOpen = false;
    this.elRef.nativeElement.focus({ preventScroll: true });
    this.soundService.play(Sound.Check);
    this.onChangeCallback(selectedOption);
    this.optSelect.emit(selectedOption);
    this.onTouchedCallback();
  }

  get selectedLabel(): string {
    const selectedOption = this.options.getValue()[this.selected];
    if (selectedOption) {
      return selectedOption.text ?? selectedOption.value;
    } else {
      return this.placeholder;
    }
  }

  toggle() {
    if (this.isDisabled || !this.options.value.length) {
      return;
    }
    this.soundService.play(this.isOpen ? Sound.Cancel : Sound.Focus);

    this.isOpen = !this.isOpen;
    this.highlighted = this.selected >= 0 ? this.selected : 0;
    this.elRef.nativeElement.focus({ preventScroll: true });
    this.cdr.detectChanges();
    if (this.isOpen) this.revealOption();
  }

  private revealOption(): void {
    const option = this.elRef.nativeElement.querySelector(`#li-${this.dropdownCounter}-${this.highlighted}`) as HTMLElement | null;
    const list = option?.parentElement;
    if (!option || !list) return;
    // Keep scrolling inside the popup; scrollIntoView also moves the lobby panel.
    const top = option.offsetTop;
    if (top < list.scrollTop) list.scrollTop = top;
    else if (top + option.offsetHeight > list.scrollTop + list.clientHeight)
      list.scrollTop = top + option.offsetHeight - list.clientHeight;
  }

  @HostListener('document:click', ['$event'])
  onClick(e: PointerEvent) {
    const target = e.target as Element;
    const thisElement = this.elRef.nativeElement as Element;

    if (this.isOpen && !thisElement.contains(target)) {
      this.soundService.play(Sound.Cancel);
      this.isOpen = false;
      this.highlighted = this.selected ?? -1;
    }
  }

  writeValue(obj: any): void {
    this.currentValue = obj;
    this.selected = this.options.getValue().findIndex(o => o.value === obj);
    this.highlighted = this.selected;
  }

  registerOnChange(fn: any) {
    this.onChangeCallback = fn;
  }

  registerOnTouched(fn: any) {
    this.onTouchedCallback = fn;
  }

  setDisabledState?(isDisabled: boolean): void {
    this.isDisabled = isDisabled;
    this.isOpen = false;
    this.cdr.detectChanges();
  }
}
