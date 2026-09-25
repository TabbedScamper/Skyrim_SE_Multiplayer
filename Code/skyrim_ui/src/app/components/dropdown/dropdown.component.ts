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
  @HostBinding('attr.tabindex') tabindex = 0;
  @HostBinding('attr.role') role = 'combobox';
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
        this.cdr.detectChanges();
      });
  }

  @HostListener('focus')
  focusHandler() {
    if (this.selected === undefined || this.selected < 0)
      this.selected = 0;
    this.isFocused = true;
  }

  @HostListener('focusout')
  focusOutHandler() {
    this.isFocused = false;
  }

  @HostListener('keydown', ['$event'])
  keydownHandler(event: KeyboardEvent) {
    if (this.isDisabled) return;

    if (event.key === 'Escape' || event.key === 'Esc') {
      // Close only the list; the menu behind it stays open.
      if (!this.isOpen) return;
      this.toggle();
    } else if (event.key === ' ' || event.key === 'Enter') {
      if (this.isOpen && this.selected >= 0) {
        const option = this.options.getValue()[this.selected];
        if (option) this.optionSelect(option.value, this.selected);
      } else {
        this.toggle();
      }
    } else if (event.key === 'ArrowUp' || event.key === 'ArrowDown') {
      if (!this.isOpen) return;
      const options = this.options.getValue();
      if (!options.length) return;
      const direction = event.key === 'ArrowDown' ? 1 : -1;
      const current = this.selected >= 0 ? this.selected : 0;
      this.selected = (current + direction + options.length) % options.length;
      document
        .querySelector(`#li-${this.dropdownCounter}-${this.selected}`)
        ?.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
    } else if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') {
      const options = this.options.getValue();
      if (!options.length) return;
      const direction = event.key === 'ArrowRight' ? 1 : -1;
      const current = this.selected >= 0 ? this.selected : 0;
      const next = (current + direction + options.length) % options.length;
      this.optionSelect(options[next].value, next);
    } else {
      return;
    }

    event.preventDefault();
    event.stopPropagation();
  }

  optionSelect(selectedOption: any, idx: number) {
    this.currentValue = selectedOption;
    this.selected = idx;
    this.isOpen = false;
    this.soundService.play(Sound.Check);
    this.onChangeCallback(selectedOption);
    this.optSelect.emit(selectedOption);
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
    if (this.isDisabled) {
      return;
    }
    this.soundService.play(this.isOpen ? Sound.Cancel : Sound.Focus);

    this.isOpen = !this.isOpen;
    if (this.selected >= 0) {
      document
        .querySelector(`#li-${this.dropdownCounter}-${this.selected}`)
        .scrollIntoView({ behavior: 'smooth', block: 'nearest' });
    }
  }

  @HostListener('document:click', ['$event'])
  onClick(e: PointerEvent) {
    const target = e.target as Element;
    const thisElement = this.elRef.nativeElement as Element;

    if (this.isOpen && !thisElement.contains(target)) {
      this.soundService.play(Sound.Cancel);
      this.isOpen = false;
    }
  }

  writeValue(obj: any): void {
    this.currentValue = obj;
    if (obj != null) {
      this.selected = this.options.getValue().findIndex(o => o.value === obj);
    }
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
