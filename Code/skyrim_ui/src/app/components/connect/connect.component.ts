import {
  AfterViewInit,
  Component,
  ElementRef,
  EventEmitter,
  HostListener,
  OnDestroy,
  Output,
  ViewChild,
} from '@angular/core';
import { TranslocoService } from '@ngneat/transloco';
import { firstValueFrom, Observable, Subscription } from 'rxjs';
import { View } from '../../models/view.enum';
import { ClientService, SavedServer, SavedServerList } from '../../services/client.service';
import { ErrorService } from '../../services/error.service';
import { Sound, SoundService } from '../../services/sound.service';
import { UiRepository } from '../../store/ui.repository';

interface PendingServerSave {
  request: number;
  id: number;
  name: string;
  address: string;
  password: string;
  remember: boolean;
  join: boolean;
}

let nextSaveRequest = 1;

@Component({
  selector: 'app-connect',
  templateUrl: './connect.component.html',
  styleUrls: ['./connect.component.scss'],
})
export class ConnectComponent implements OnDestroy, AfterViewInit {
  public serverName = '';
  public selectedServerId = 0;
  public savedServers$: Observable<SavedServerList>;
  public address = '';
  public password = '';
  public savePassword = false;
  public hidePassword = true;

  public connecting = false;
  private rejoining = false;
  public pendingSave: PendingServerSave | undefined;

  @Output() public done = new EventEmitter<void>();

  public constructor(
    private readonly client: ClientService,
    private readonly sound: SoundService,
    private readonly errorService: ErrorService,
    private readonly translocoService: TranslocoService,
    private readonly uiRepository: UiRepository,
  ) {
    this.savedServers$ = this.client.savedServers.asObservable();
    this.connectionSubscription = this.client.connectionStateChange.subscribe(
      async state => {
        if (this.connecting) {
          this.connecting = false;

          if (state) {
            this.sound.play(Sound.Success);
            this.client.refreshSavedServers();
            this.done.next();
          } else if (this.errorService.getError() === '') {
            // show connection error when there is no more specific error
            const message = await firstValueFrom(
              this.translocoService.selectTranslate<string>(
                'COMPONENT.CONNECT.ERROR.CONNECTION',
              ),
            );
            await this.errorService.setError(message, 'SERVICE.ERROR.TITLES.CONNECT');
          }
        }
      },
    );

    this.protocolMismatchSubscription =
      this.client.protocolMismatchChange.subscribe(async state => {
        if (state) {
          this.connecting = false;
          const message = await firstValueFrom(
            this.translocoService.selectTranslate<string>(
              'COMPONENT.CONNECT.ERROR.VERSION_MISMATCH',
            ),
          );
          await this.errorService.setError(message, 'SERVICE.ERROR.TITLES.CONNECT');
        }
      });

    this.savedListSubscription = this.client.savedServers.subscribe(list => {
      const pending = this.pendingSave;
      if (!pending || pending.request !== list.request) return;
      if (list.error) {
        this.pendingSave = undefined;
        const storageError = /could not (read|write) servers.txt|LOCALAPPDATA|saved server list is busy/.test(list.error);
        const fullNewList = !pending.id && list.servers.length >= 64;
        if (pending.join && !pending.remember && (storageError || fullNewList)) {
          skyrimtogether.showHudMessage('Could not save server: ' + list.error);
          const endpoint = pending.address.match(/^(.+?)(?::([0-9]+))?$/);
          this.client.connect(endpoint[1], endpoint[2] ? Number(endpoint[2]) : 10578, pending.password);
        } else {
          this.connecting = false;
        }
        return;
      }
      const server = list.servers.find(item => item.id === list.savedId && item.name === pending.name && this.sameEndpoint(item.address, pending.address));
      if (!server) {
        this.pendingSave = undefined;
        this.connecting = false;
        this.client.refreshSavedServers();
        return;
      }
      this.pendingSave = undefined;
      this.selectedServerId = server.id;
      this.password = '';
      if (pending.join) {
        this.client.connectSavedServer(server.id, pending.remember ? undefined : pending.password);
      }
    });
    this.client.refreshSavedServers();

    // Rejoin without a remembered credential opens the same form for password entry.
    const rejoin = this.uiRepository.takeConnectNow();
    if (typeof rejoin === 'object' && rejoin) {
      const server = this.client.savedServers.value.servers.find(item => item.id === rejoin.savedId);
      if (server) this.editSaved(server);
    } else if (typeof rejoin === 'string' && rejoin.trim()) {
      this.address = rejoin;
      this.rejoining = true;
    }
  }

  public ngAfterViewInit(): void {
    if (this.rejoining) {
      setTimeout(() => this.connect());
      return;
    }

    setTimeout(() => {
      this.inputRef?.nativeElement.focus();
    }, 100);
  }

  public ngOnDestroy(): void {
    this.connectionSubscription.unsubscribe();
    this.protocolMismatchSubscription.unsubscribe();
    this.savedListSubscription.unsubscribe();
  }

  async connect(): Promise<void> {
    if (this.pendingSave) return;
    const address = this.address.trim().match(/^(.+?)(?::([0-9]+))?$/);

    if (!address || /[\s\x00-\x1f\x7f]/.test(this.address.trim())) {
      this.sound.play(Sound.Fail);
      const message = await firstValueFrom(
        this.translocoService.selectTranslate(
          'COMPONENT.CONNECT.ERROR.INVALID_ADDRESS',
        ),
      );
      await this.errorService.setError(message, 'SERVICE.ERROR.TITLES.CONNECT');
      return;
    }

    const port = address[2] ? Number.parseInt(address[2]) : 10578;
    if (port < 1 || port > 65535) {
      await this.errorService.setError('COMPONENT.CONNECT.ERROR.INVALID_ADDRESS', 'SERVICE.ERROR.TITLES.CONNECT');
      return;
    }
    this.connecting = true;
    this.sound.play(Sound.Ok);
    this.saveToNative(true);
  }

  private sameEndpoint(left: string, right: string): boolean {
    const endpoint = (value: string) => {
      const trimmed = value.trim().toLowerCase();
      return trimmed.includes(':') ? trimmed : trimmed + ':10578';
    };
    return endpoint(left) === endpoint(right);
  }

  private saveToNative(join: boolean): void {
    if (this.pendingSave) return;
    const typedAddress = this.address.trim();
    const current = this.client.savedServers.value.servers.find(server =>
      this.selectedServerId ? server.id === this.selectedServerId : this.sameEndpoint(server.address, typedAddress),
    );
    const address = current && this.sameEndpoint(current.address, typedAddress) ? current.address : typedAddress;
    const name = this.serverName.trim() || current?.name || address;
    const request = nextSaveRequest++;
    this.pendingSave = { request, id: current?.id || 0, name, address, password: this.password, remember: this.savePassword, join };
    skyrimtogether.saveServer(current?.id || 0, name, address, this.password, this.savePassword, request);
  }

  public editSaved(server: SavedServer): void {
    this.selectedServerId = server.id;
    this.serverName = server.name;
    this.address = server.address;
    this.password = '';
    this.savePassword = server.hasPassword;
  }

  public newServer(): void {
    this.selectedServerId = 0;
    this.serverName = '';
    this.address = '';
    this.password = '';
    this.savePassword = false;
  }

  public saveServer(): void {
    this.saveToNative(false);
  }

  public removeSaved(): void {
    if (this.selectedServerId) {
      skyrimtogether.removeSavedServer(this.selectedServerId);
      this.newServer();
    }
  }

  public importServer(): void {
    skyrimtogether.importSavedServer();
  }

  public joinSaved(server: SavedServer): void {
    if (this.pendingSave) return;
    this.address = server.address;
    this.connecting = true;
    this.client.connectSavedServer(server.id);
  }

  public cancel(): void {
    this.sound.play(Sound.Cancel);
    this.done.next();
  }

  /** Stops the attempt and goes back to the form; set before disconnecting, so the disconnection reads as no failure. */
  public cancelConnecting(): void {
    this.pendingSave = undefined;
    this.connecting = false;
    this.client.disconnect();
    this.client.isConnectionInProgressChange.next(false);
    this.sound.play(Sound.Cancel);
  }

  public openServerList(): void {
    this.uiRepository.openView(View.SERVER_LIST);
  }

  @ViewChild('input')
  private inputRef!: ElementRef;

  private connectionSubscription: Subscription;

  private protocolMismatchSubscription: Subscription;

  private savedListSubscription: Subscription;

  @HostListener('window:keydown.escape', ['$event'])
  // @ts-ignore
  private activate(event: KeyboardEvent): void {
    if (this.errorService.getError()) {
      this.errorService.removeError();
    } else if (this.connecting) {
      this.cancelConnecting();
    } else {
      this.done.next();
    }

    event.stopPropagation();
    event.preventDefault();
  }
}
