import { Provider } from '@angular/core';

export const environment = {
  production: true,
  game: true,
  urlProtocol: 'https',
  url: '',
  githubUrl: 'https://api.github.com/repos/TabbedScamper/Skyrim_SE_Multiplayer/tags',
  overwriteVersion: "",
  chatMessageLengthLimit: 512,
  nbReconnectionAttempts: 5,

  providers: [] as Provider[],
};
