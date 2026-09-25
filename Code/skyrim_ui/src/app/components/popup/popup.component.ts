import { Component } from '@angular/core';
import { animation } from './popup.animation';

@Component({
  selector: 'app-popup',
  templateUrl: './popup.component.html',
  styleUrls: ['./popup.component.scss'],
  animations: [animation],
  // Every popup is a controller navigation surface (GamepadNavigationService).
  host: { '[@popup]': 'true', 'data-nav-scope': '' },
})
export class PopupComponent {}
