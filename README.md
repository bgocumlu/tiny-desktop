# Tiny Desktop

Build lightweight cross-platform desktop apps with HTML, CSS, and JavaScript. `tiny-desktop` is an npm-first desktop runtime and build tool for Windows, Linux, and macOS. `create-tiny-desktop` scaffolds a Vite project.

Only Node.js and npm are required.

## Quick start

```bash
npx create-tiny-desktop my-app --no-install
cd my-app
npm install
npm run dev
npm run build
```

Omit `--no-install` to let the generator run `npm install` automatically.

The generated app uses Vite. Edit `src/`, then use `npm run dev` for development and `npm run build` to create a release in `release/`.

## Configure the app

Edit `tiny.config.js`:

```js
export default {
  app: {
    name: 'My App',
    version: '1.0.0',
    publisher: 'My Company',
    description: 'A small desktop app'
  },
  package: 'standalone', // or 'installer'
  storage: { mode: 'appData' }, // or 'portable'
  window: { width: 1000, height: 700 }
};
```

`standalone` creates a directly runnable executable. `installer` creates the platform installer: NSIS on Windows, `.deb` on Linux, and `.dmg` on macOS.

## Native API

The generated app can use `window.tiny`:

| API | Purpose |
| --- | --- |
| `tiny.app.getDataPath()` | Get the app data directory |
| `tiny.data.read(store)` / `write(store, value)` / `remove(store)` | Read, write, and remove JSON data |
| `tiny.window.close()` / `minimize()` / `maximize()` / `restore()` | Control the window |
| `tiny.shell.openExternal(url)` | Open a URL with the system browser |

## Platform requirements

- Windows: WebView2 Runtime; the Windows installer includes a bootstrapper.
- Linux: GTK 3 and WebKitGTK 4.1 runtime packages, typically `libgtk-3-0` and `libwebkit2gtk-4.1-0`.
- macOS: the built-in WebKit runtime.

The runtime is packaged for `win32-x64`, `linux-x64`, `darwin-x64`, and `darwin-arm64`.
