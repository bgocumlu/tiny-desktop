export default {
  app: {
    name: '__TINY_APP_NAME__',
    version: '0.1.0',
    publisher: '',
    description: '__TINY_APP_NAME__',
    copyright: '',
    website: '',
    // Replace assets/icon.ico with your own Windows icon.
    icon: './assets/icon.ico'
  },
  package: 'standalone',
  storage: {
    mode: 'appData'
  },
  window: {
    titleBar: {
      color: '#202020',
      textColor: '#ffffff'
    }
  }
};
