export default {
  app: {
    name: '__TINY_APP_NAME__',
    version: '0.1.0',
    publisher: '',
    maintainer: '',
    description: '__TINY_APP_NAME__',
    copyright: '',
    website: '',
    icon: {
      win32: './assets/icon.ico',
      linux: './assets/icon.svg',
      darwin: './assets/icon.icns',
      default: './assets/icon.ico'
    }
  },
  package: 'standalone',
  storage: {
    mode: 'appData'
  },
  window: {
    width: 1200,
    height: 800,
    titleBar: {
      color: '#202020',
      textColor: '#ffffff'
    }
  }
};
