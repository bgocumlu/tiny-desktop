#!/usr/bin/env node
import { cp, mkdir, readdir, readFile, rm, stat, writeFile } from 'node:fs/promises';
import { spawn } from 'node:child_process';
import { dirname, basename, extname, join, relative, resolve, sep } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import * as ResEdit from 'resedit';

const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const projectRoot = resolve(process.cwd());
const nativeBuild = join(packageRoot, 'native', 'build');
const runtime = join(packageRoot, 'runtime', 'win32-x64', 'tiny-host.exe');
const nsis = join(packageRoot, 'runtime', 'win32-x64', 'nsis', 'Bin', 'makensis.exe');
const webviewBootstrapper = join(packageRoot, 'runtime', 'win32-x64', 'installer', 'MicrosoftEdgeWebView2Setup.exe');
const release = join(projectRoot, 'release');
const vite = join(projectRoot, 'node_modules', 'vite', 'bin', 'vite.js');

function run(command, args, options = {}) {
  return new Promise((resolvePromise, reject) => {
    const child = spawn(command, args, { cwd: projectRoot, stdio: 'inherit', ...options });
    child.on('error', reject);
    child.on('exit', (code, signal) => {
      if (signal) reject(new Error(`${command} stopped with ${signal}`));
      else if (code) reject(new Error(`${command} exited with ${code}`));
      else resolvePromise();
    });
  });
}

async function loadConfig() {
  const configPath = join(projectRoot, 'tiny.config.js');
  const defaults = { app: { name: basename(projectRoot) }, package: 'standalone', storage: { mode: 'appData' }, window: { titleBar: {} } };
  try {
    await stat(configPath);
  } catch {
    return defaults;
  }
  const loaded = await import(`${pathToFileURL(configPath).href}?v=${Date.now()}`);
  const config = loaded.default ?? loaded;
  const packageMode = config.package ?? defaults.package;
  if (packageMode !== 'standalone' && packageMode !== 'installer') {
    throw new Error("tiny.config.js package must be 'standalone' or 'installer'.");
  }
  const mode = config.storage?.mode ?? defaults.storage.mode;
  if (mode !== 'appData' && mode !== 'portable') throw new Error("tiny.config.js storage.mode must be 'appData' or 'portable'.");
  const icon = config.app?.icon ? resolve(projectRoot, String(config.app.icon)) : undefined;
  if (icon && extname(icon).toLowerCase() !== '.ico') throw new Error("tiny.config.js app.icon must point to an .ico file.");
  for (const [name, value] of Object.entries(config.window?.titleBar ?? {})) {
    if (['color', 'textColor'].includes(name) && value !== undefined && !/^#[0-9a-f]{6}$/i.test(String(value))) {
      throw new Error(`tiny.config.js window.titleBar.${name} must be a #RRGGBB color.`);
    }
  }
  return {
    ...defaults,
    ...config,
    package: packageMode,
    app: { ...defaults.app, ...config.app, name: String(config.app?.name ?? defaults.app.name), icon },
    storage: { mode },
    window: { ...defaults.window, ...config.window, titleBar: { ...defaults.window.titleBar, ...config.window?.titleBar } }
  };
}

async function requireVite() {
  await stat(vite).catch(() => {
    throw new Error(`Vite is not installed in ${projectRoot}. Run npm install there first.`);
  });
}

async function requireRuntime() {
  if (process.platform !== 'win32' || process.arch !== 'x64') {
    throw new Error('Tiny V1 currently supports Windows x64 only.');
  }
  await stat(runtime).catch(() => {
    throw new Error('The Windows runtime is missing. Run npm run stage-runtime in the Tiny repository.');
  });
}

async function requireInstaller() {
  if (process.platform !== 'win32' || process.arch !== 'x64') {
    throw new Error('Tiny V1 installer currently supports Windows x64 only.');
  }
  await stat(nsis).catch(() => {
    throw new Error('The Tiny NSIS toolchain is missing from the runtime package.');
  });
  await stat(webviewBootstrapper).catch(() => {
    throw new Error('The WebView2 bootstrapper is missing from the runtime package.');
  });
}

async function buildNative() {
  await run('cmake', ['-S', 'native', '-B', 'native/build', '-A', 'x64'], { cwd: packageRoot });
  await run('cmake', ['--build', 'native/build', '--config', 'Release'], { cwd: packageRoot });
  return join(nativeBuild, 'Release', 'tiny-host.exe');
}

async function stageRuntime() {
  const host = await buildNative();
  await mkdir(dirname(runtime), { recursive: true });
  await mkdir(join(packageRoot, '.local-packages'), { recursive: true });
  await cp(host, runtime);
  console.log(`Staged ${relative(packageRoot, runtime)}.`);
}

async function waitForVite(url, timeout = 30000) {
  const started = Date.now();
  while (Date.now() - started < timeout) {
    try {
      await fetch(url);
      return;
    } catch {
      await new Promise((resolvePromise) => setTimeout(resolvePromise, 200));
    }
  }
  throw new Error(`Vite did not start at ${url}`);
}

function runtimeArgs(config) {
  const args = ['--app-name', config.app.name, '--storage', config.storage.mode];
  if (config.storage.mode === 'portable') args.push('--data-dir', join(projectRoot, '.tiny', 'data'));
  const titleBar = config.window.titleBar;
  if (titleBar.color) args.push('--titlebar-color', titleBar.color);
  if (titleBar.textColor) args.push('--titlebar-text-color', titleBar.textColor);
  return args;
}

async function startDev() {
  await requireVite();
  await requireRuntime();
  const config = await loadConfig();
  const url = 'http://127.0.0.1:5173/';
  const viteProcess = spawn(process.execPath, [vite, '--host', '127.0.0.1', '--port', '5173', '--strictPort'], {
    cwd: projectRoot,
    stdio: 'inherit',
    windowsHide: true
  });
  let hostProcess;
  const stop = () => {
    hostProcess?.kill();
    viteProcess.kill();
  };
  process.once('SIGINT', stop);
  process.once('SIGTERM', stop);

  try {
    await waitForVite(url);
    hostProcess = spawn(runtime, ['--dev', url, '--devtools', ...runtimeArgs(config)], {
      cwd: projectRoot,
      stdio: 'ignore'
    });
    await new Promise((resolvePromise, reject) => {
      hostProcess.once('error', reject);
      hostProcess.once('exit', (code) => code ? reject(new Error(`tiny-host exited with ${code}`)) : resolvePromise());
    });
  } finally {
    stop();
    process.removeListener('SIGINT', stop);
    process.removeListener('SIGTERM', stop);
  }
}

async function collectFiles(directory, prefix = '') {
  const files = [];
  for (const entry of await readdir(directory, { withFileTypes: true })) {
    const name = join(prefix, entry.name);
    if (entry.isDirectory()) files.push(...await collectFiles(join(directory, entry.name), name));
    else files.push({ path: name.split(sep).join('/'), bytes: await readFile(join(directory, entry.name)) });
  }
  return files;
}

async function setIcon(executable, icon) {
  const binary = ResEdit.NtExecutable.from(await readFile(executable));
  const resources = ResEdit.NtExecutableResource.from(binary);
  const group = ResEdit.Resource.IconGroupEntry.fromEntries(resources.entries)[0];
  if (!group) throw new Error('The Tiny host has no icon resource to replace.');
  const iconFile = ResEdit.Data.IconFile.from(await readFile(icon));
  ResEdit.Resource.IconGroupEntry.replaceIconsForResource(
    resources.entries,
    group.id,
    group.lang,
    iconFile.icons.map(({ data }) => data)
  );
  resources.outputResource(binary);
  await writeFile(executable, Buffer.from(binary.generate()));
}

async function bundleRuntime(host, dist, output, config) {
  const hostBytes = await readFile(host);
  const files = await collectFiles(dist);
  let offset = 0;
  const entries = files.map(({ path, bytes }) => {
    const entry = { path, offset, size: bytes.length };
    offset += bytes.length;
    return entry;
  });
  const manifest = Buffer.from(JSON.stringify({ appName: config.app.name, storage: config.storage.mode, files: entries }));
  const header = Buffer.alloc(16);
  header.write('TINYBND1', 0, 'ascii');
  header.writeBigUInt64LE(BigInt(manifest.length), 8);
  const payload = Buffer.concat(files.map(({ bytes }) => bytes));
  const footer = Buffer.alloc(16);
  footer.write('TINYEND1', 0, 'ascii');
  footer.writeBigUInt64LE(BigInt(hostBytes.length), 8);
  await writeFile(output, Buffer.concat([hostBytes, header, manifest, payload, footer]));
}

function nsisString(value) {
  return String(value)
    .replaceAll('$', '$$')
    .replaceAll('"', '$\\"')
    .replace(/[\r\n]/g, ' ');
}

function installerScript(config, executable, output, icon) {
  const appName = nsisString(config.app.name);
  const appFile = nsisString(executable);
  const installerFile = nsisString(output);
  const bootstrapperFile = nsisString(webviewBootstrapper);
  const iconLine = icon ? `Icon "${nsisString(icon)}"\nUninstallIcon "${nsisString(icon)}"` : '';
  const uninstallKey = `Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${appName}`;
  const installDir = `$LOCALAPPDATA\\Programs\\${appName}`;
  const executableName = nsisString(executable.split(/[\\/]/).pop());

  return `Unicode True
!include nsDialogs.nsh
!include WinMessages.nsh
Name "${appName}"
Caption "${appName} Setup"
OutFile "${installerFile}"
InstallDir "${installDir}"
InstallDirRegKey HKCU "${uninstallKey}" "InstallLocation"
RequestExecutionLevel user
SetCompressor /SOLID lzma
${iconLine}
BrandingText "${appName}"
Page custom WebView2PageCreate WebView2PageLeave
PageEx directory
  PageCallbacks "" DirectoryPageShow ""
PageExEnd
Page instfiles
UninstPage custom un.UninstallPageCreate un.UninstallPageLeave
UninstPage instfiles

Var WebView2Version
Var WebView2Result
Var WebView2InstallButton
Var WebView2Status
Var DeleteDataCheckbox
Var DeleteDataState

Function IsWebView2Installed
  StrCpy $WebView2Result 0
  SetRegView 64
  ReadRegStr $WebView2Version HKLM "SOFTWARE\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}" "pv"
  StrCmp $WebView2Version "" +2 0
    StrCpy $WebView2Result 1
  StrCmp $WebView2Result 1 webview_registry_done
  ReadRegStr $WebView2Version HKCU "Software\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}" "pv"
  StrCmp $WebView2Version "" +2 0
    StrCpy $WebView2Result 1
  SetRegView 32
  StrCmp $WebView2Result 1 webview_registry_done
  ReadRegStr $WebView2Version HKLM "SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}" "pv"
  StrCmp $WebView2Version "" +2 0
    StrCpy $WebView2Result 1
  StrCmp $WebView2Result 1 webview_registry_done
  ReadRegStr $WebView2Version HKCU "Software\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}" "pv"
  StrCmp $WebView2Version "" +2 0
    StrCpy $WebView2Result 1
webview_registry_done:
FunctionEnd

Function WebView2PageCreate
  Call IsWebView2Installed
  StrCmp $WebView2Result 1 webview_page_skip
  InitPluginsDir
  File /oname=$PLUGINSDIR\\MicrosoftEdgeWebView2Setup.exe "${bootstrapperFile}"
  nsDialogs::Create 1018
  Pop $0
  \${NSD_CreateLabel} 0 0 100% 24u "${appName} requires Microsoft Edge WebView2 Runtime to run."
  Pop $0
  \${NSD_CreateLabel} 0 28u 100% 36u "WebView2 was not found on this computer. Install it before continuing."
  Pop $0
  \${NSD_CreateButton} 0 76u 110u 14u "Install WebView2"
  Pop $WebView2InstallButton
  \${NSD_OnClick} $WebView2InstallButton WebView2Install
  \${NSD_CreateLabel} 0 104u 100% 28u ""
  Pop $WebView2Status
  nsDialogs::Show
  Return

webview_page_skip:
  Abort
FunctionEnd

Function WebView2Install
  EnableWindow $WebView2InstallButton 0
  ExecWait '"$PLUGINSDIR\\MicrosoftEdgeWebView2Setup.exe" /install' $0
  Call IsWebView2Installed
  StrCmp $WebView2Result 1 webview_install_success
  \${NSD_SetText} $WebView2Status "WebView2 was not installed. Try again or click Cancel."
  EnableWindow $WebView2InstallButton 1
  Return

webview_install_success:
  \${NSD_SetText} $WebView2Status "WebView2 is installed. Click Next to continue."
FunctionEnd

Function WebView2PageLeave
  Call IsWebView2Installed
  StrCmp $WebView2Result 1 webview_page_ready
  MessageBox MB_ICONEXCLAMATION|MB_OK "${appName} cannot run without Microsoft Edge WebView2 Runtime."
  Abort

webview_page_ready:
FunctionEnd

Function DirectoryPageShow
  GetDlgItem $0 $HWNDPARENT 3
  ShowWindow $0 \${SW_HIDE}
FunctionEnd

Function un.UninstallPageCreate
  nsDialogs::Create 1018
  Pop $0
  \${NSD_CreateLabel} 0 0 100% 24u "This wizard will uninstall ${appName}."
  Pop $0
  \${NSD_CreateLabel} 0 28u 100% 32u "Uninstalling from: $INSTDIR"
  Pop $0
  \${NSD_CreateCheckbox} 0 72u 100% 14u "Delete all ${appName} data"
  Pop $DeleteDataCheckbox
  nsDialogs::Show
FunctionEnd

Function un.UninstallPageLeave
  \${NSD_GetState} $DeleteDataCheckbox $DeleteDataState
FunctionEnd

Section
  SetOutPath "$INSTDIR"
  File "${appFile}"

  WriteUninstaller "$INSTDIR\\Uninstall.exe"
  WriteRegStr HKCU "${uninstallKey}" "DisplayName" "${appName}"
  WriteRegStr HKCU "${uninstallKey}" "DisplayIcon" "$INSTDIR\\${executableName}"
  WriteRegStr HKCU "${uninstallKey}" "UninstallString" "$INSTDIR\\Uninstall.exe"
  WriteRegStr HKCU "${uninstallKey}" "InstallLocation" "$INSTDIR"
  CreateDirectory "$SMPROGRAMS\\${appName}"
  CreateShortcut "$SMPROGRAMS\\${appName}\\${appName}.lnk" "$INSTDIR\\${executableName}"
  CreateShortcut "$DESKTOP\\${appName}.lnk" "$INSTDIR\\${executableName}"
SectionEnd

Section "Uninstall"
  Delete "$DESKTOP\\${appName}.lnk"
  Delete "$SMPROGRAMS\\${appName}\\${appName}.lnk"
  RMDir "$SMPROGRAMS\\${appName}"
  Delete "$INSTDIR\\${executableName}"
  Delete "$INSTDIR\\Uninstall.exe"
  RMDir "$INSTDIR"
  StrCmp $DeleteDataState 1 delete_app_data keep_app_data
delete_app_data:
  RMDir /r "$LOCALAPPDATA\\${appName}"
  RMDir /r "$APPDATA\\${appName}"
keep_app_data:
  DeleteRegKey HKCU "${uninstallKey}"
SectionEnd
`;
}

async function buildStandalone(config) {
  await requireVite();
  await requireRuntime();
  await run(process.execPath, [vite, 'build']);
  await mkdir(release, { recursive: true });
  const filename = (config.app.name.replace(/[<>:"/\\|?*]/g, '-').trim() || 'Tiny') + '.exe';
  const output = join(release, filename);
  const iconHost = config.app.icon ? join(release, '.tiny-host.exe') : undefined;
  await rm(join(release, 'app'), { recursive: true, force: true });
  await rm(output, { force: true });
  if (iconHost) {
    await cp(runtime, iconHost);
    await setIcon(iconHost, config.app.icon);
  }
  try {
    await bundleRuntime(iconHost ?? runtime, join(projectRoot, 'dist'), output, config);
  } finally {
    if (iconHost) await rm(iconHost, { force: true });
  }
  console.log(`Built release/${filename} with embedded Vite assets.`);
  return output;
}

async function buildInstaller(executable, config) {
  await requireInstaller();
  const output = join(release, `${basename(executable, '.exe')}-setup.exe`);
  const script = join(release, '.tiny-installer.nsi');
  const icon = config.app.icon ? join(release, '.tiny-installer.ico') : undefined;
  if (icon) await cp(config.app.icon, icon);
  await rm(output, { force: true });
  await writeFile(script, installerScript(config, executable, output, icon));
  try {
    await run(nsis, [script], { cwd: release, windowsHide: true });
  } finally {
    await rm(script, { force: true });
    if (icon) await rm(icon, { force: true });
  }
  console.log(`Built release/${basename(output)} with NSIS.`);
  return output;
}

async function buildApp() {
  const config = await loadConfig();
  const executable = await buildStandalone(config);
  if (config.package === 'installer') await buildInstaller(executable, config);
}

async function check() {
  await run(process.execPath, ['--check', 'cli/tiny.mjs'], { cwd: packageRoot });
  await run(process.execPath, ['--check', 'create-tiny-app/cli/create.mjs'], { cwd: packageRoot });
  await stageRuntime();
  console.log('Checks passed.');
}

const command = process.argv[2] ?? 'help';
try {
  if (command === 'dev') await startDev();
  else if (command === 'build') await buildApp();
  else if (command === 'check') await check();
  else if (command === 'stage-runtime') await stageRuntime();
  else console.log('Usage: tiny dev | tiny build');
} catch (error) {
  console.error(error.message);
  process.exitCode = 1;
}
