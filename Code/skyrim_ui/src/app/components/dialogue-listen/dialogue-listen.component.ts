import { CommonModule } from '@angular/common';
import { Component, ElementRef, NgZone, OnDestroy } from '@angular/core';

@Component({
  selector: 'app-dialogue-listen',
  standalone: true,
  imports: [CommonModule],
  templateUrl: './dialogue-listen.component.html',
  styleUrls: ['./dialogue-listen.component.scss'],
})
export class DialogueListenComponent implements OnDestroy {
  visible = false;
  speaker = 0;
  topics: Array<{ index: number; text: string; said: boolean }> = [];
  highlighted = -1;
  chosen = -1;
  chosenText = '';
  subtitle = '';
  flashing = false;
  private choiceSerial = '0';
  private lineSerial = '0';
  private flashTimer?: ReturnType<typeof setTimeout>;
  private subtitleTimer?: ReturnType<typeof setTimeout>;
  private scrollFrame = 0;

  private readonly receive: SkyrimTogetherTypes.DialogueListenCallback = (
    visible, speaker, topics, highlighted, chosen, choiceSerial,
    chosenText, subtitle, durationMs, lineSerial,
  ) => this.zone.run(() => {
    this.visible = visible;
    if (!visible) { this.clear(); return; }
    this.speaker = speaker;
    this.topics = topics.map(([index, text, said]) => ({ index, text, said }));
    this.highlighted = highlighted;
    cancelAnimationFrame(this.scrollFrame);
    this.scrollFrame = requestAnimationFrame(() => {
      this.element.nativeElement.querySelector('.highlighted')?.scrollIntoView({ block: 'nearest' });
    });
    this.chosen = chosen;
    this.chosenText = chosenText;
    if (choiceSerial !== this.choiceSerial) {
      this.choiceSerial = choiceSerial;
      clearTimeout(this.flashTimer);
      this.flashing = true;
      this.flashTimer = setTimeout(() => this.flashing = false, 650);
    }
    if (lineSerial !== this.lineSerial || subtitle !== this.subtitle) {
      this.lineSerial = lineSerial;
      clearTimeout(this.subtitleTimer);
      this.subtitle = subtitle;
      if (durationMs > 0) {
        this.subtitleTimer = setTimeout(() => this.subtitle = '', durationMs);
      }
    }
  });

  private readonly reset = () => this.zone.run(() => { this.visible = false; this.clear(); });

  constructor(private readonly zone: NgZone, private readonly element: ElementRef<HTMLElement>) {
    skyrimtogether.on('dialogueListen', this.receive);
    skyrimtogether.on('disconnect', this.reset);
    skyrimtogether.on('exitGame', this.reset);
  }

  private clear(): void {
    clearTimeout(this.flashTimer);
    clearTimeout(this.subtitleTimer);
    cancelAnimationFrame(this.scrollFrame);
    this.topics = [];
    this.subtitle = '';
    this.chosenText = '';
    this.flashing = false;
    this.choiceSerial = this.lineSerial = '0';
  }

  ngOnDestroy(): void {
    this.clear();
    skyrimtogether.off('dialogueListen', this.receive);
    skyrimtogether.off('disconnect', this.reset);
    skyrimtogether.off('exitGame', this.reset);
  }
}
