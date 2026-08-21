import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import { mkdtemp, readFile, rm, stat, writeFile } from 'node:fs/promises';
import { basename, dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import * as ResEdit from 'resedit';

if (process.platform !== 'win32' || process.arch !== 'x64') {
  throw new Error('Tiny Windows e2e requires Windows x64.');
}

const repoRoot = resolve(fileURLToPath(new URL('..', import.meta.url)));
const npmCli = process.env.npm_execpath;
if (!npmCli) throw new Error('Run the e2e test with npm test.');
const projectRoot = await mkdtemp(join(repoRoot, '.tmp-e2e-'));
const appName = basename(projectRoot).replace(/^\.+/, '');
const dataRoot = join(process.env.APPDATA, appName);
const webviewRoot = join(process.env.LOCALAPPDATA, appName, 'WebView2');
let host;
let passed = false;

function run(args, cwd, env = process.env) {
  const result = spawnSync(process.execPath, [npmCli, ...args], {
    cwd,
    env,
    stdio: 'inherit',
    windowsHide: true
  });
  if (result.error) throw result.error;
  assert.equal(result.status, 0, `npm ${args.join(' ')} failed with ${result.status}`);
}

async function exists(path) {
  try {
    await stat(path);
    return true;
  } catch {
    return false;
  }
}

async function waitFor(check, timeoutMs = 15000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (await check()) return;
    await new Promise(resolvePromise => setTimeout(resolvePromise, 200));
  }
  throw new Error('Timed out waiting for the Tiny app to finish starting.');
}

async function waitForExit(child, timeoutMs = 5000) {
  if (child.exitCode !== null) return;
  await new Promise(resolvePromise => {
    const timer = setTimeout(resolvePromise, timeoutMs);
    child.once('exit', () => {
      clearTimeout(timer);
      resolvePromise();
    });
  });
}

try {
  run(['run', 'pack:local'], repoRoot);
  run(['run', 'pack:create-local'], repoRoot);

  const runtimeVersion = JSON.parse(await readFile(join(repoRoot, 'package.json'), 'utf8')).version;
  const createVersion = JSON.parse(await readFile(join(repoRoot, 'create-tiny-desktop/package.json'), 'utf8')).version;
  const runtimePath = resolve(repoRoot, '.local-packages', `tiny-desktop-${runtimeVersion}.tgz`).replaceAll('\\', '/');
  const env = process.env;
  const createPackage = `./.local-packages/create-tiny-desktop-${createVersion}.tgz`;
  const invalidCreate = spawnSync(process.execPath, [npmCli, 'exec', '--yes', `--package=${createPackage}`, '--', 'create-tiny-desktop'], {
    cwd: repoRoot,
    env,
    encoding: 'utf8',
    windowsHide: true
  });
  assert.notEqual(invalidCreate.status, 0, 'The initializer accepted a missing directory.');
  assert.match(`${invalidCreate.stdout}\n${invalidCreate.stderr}`, /A target directory is required/);
  assert.match(`${invalidCreate.stdout}\n${invalidCreate.stderr}`, /Usage: npx create-tiny-desktop <directory>/);
  run(['exec', '--yes', `--package=${createPackage}`, '--', 'create-tiny-desktop', projectRoot, '--no-install'], repoRoot, env);

  const generatedPackagePath = join(projectRoot, 'package.json');
  const generatedPackage = JSON.parse(await readFile(generatedPackagePath, 'utf8'));
  generatedPackage.devDependencies['tiny-desktop'] = `file:${runtimePath}`;
  await writeFile(generatedPackagePath, JSON.stringify(generatedPackage, null, 2) + '\n');
  run(['install'], projectRoot, env);

  for (const path of ['package.json', '.gitignore', 'index.html', 'src/main.js', 'src/style.css', 'tiny.config.js', 'assets/icon.ico', 'assets/icon.svg']) {
    assert.equal(await exists(join(projectRoot, path)), true, `Missing generated file: ${path}`);
  }

  const configPath = join(projectRoot, 'tiny.config.js');
  let config = await readFile(configPath, 'utf8');
  config = config
    .replace("publisher: ''", "publisher: 'Tiny Test Publisher'")
    .replace(`description: '${appName}'`, "description: 'Tiny metadata test'");
  await writeFile(configPath, config);

  const npmOnlyEnv = {
    ...process.env,
    PATH: [join(projectRoot, 'node_modules', '.bin'), dirname(process.execPath)].join(';')
  };
  run(['run', 'build'], projectRoot, npmOnlyEnv);

  const executable = join(projectRoot, 'release', `${appName}.exe`);
  assert.equal(await exists(executable), true, 'The production executable was not created.');
  assert.equal(await exists(join(projectRoot, 'release', 'app')), false, 'Build left extracted assets beside the executable.');
  assert.ok((await stat(executable)).size > 100 * 1024, 'The executable is unexpectedly small.');

  const bundle = (await readFile(executable)).toString('ascii');
  assert.ok(bundle.includes('TINYBND1'), 'The executable is missing the bundle header.');
  assert.ok(bundle.includes('TINYEND1'), 'The executable is missing the bundle footer.');
  const resources = ResEdit.NtExecutableResource.from(ResEdit.NtExecutable.from(await readFile(executable)));
  const versionInfo = ResEdit.Resource.VersionInfo.fromEntries(resources.entries)[0];
  assert.ok(versionInfo, 'The executable is missing version metadata.');
  const versionValues = versionInfo.getStringValues({ lang: 1033, codepage: 1200 });
  assert.equal(versionValues.ProductName, appName, 'The executable ProductName is incorrect.');
  assert.equal(versionValues.CompanyName, 'Tiny Test Publisher', 'The executable CompanyName is incorrect.');
  assert.equal(versionValues.FileDescription, 'Tiny metadata test', 'The executable FileDescription is incorrect.');
  assert.equal(versionValues.FileVersion, '0.1.0.0', 'The executable FileVersion is incorrect.');

  await writeFile(configPath, config.replace("package: 'standalone'", "package: 'installer'"));
  run(['run', 'build'], projectRoot, npmOnlyEnv);
  const installer = join(projectRoot, 'release', `${appName}-setup.exe`);
  assert.equal(await exists(installer), true, 'The NSIS installer was not created.');
  assert.ok((await stat(installer)).size > 100 * 1024, 'The installer is unexpectedly small.');
  const installerResources = ResEdit.NtExecutableResource.from(ResEdit.NtExecutable.from(await readFile(installer)));
  const installerVersionInfo = ResEdit.Resource.VersionInfo.fromEntries(installerResources.entries)[0];
  const installerVersionValues = installerVersionInfo.getStringValues({ lang: 1033, codepage: 1200 });
  assert.equal(installerVersionValues.ProductName, appName, 'The installer ProductName is incorrect.');
  assert.equal(installerVersionValues.CompanyName, 'Tiny Test Publisher', 'The installer CompanyName is incorrect.');
  const installedRoot = join(projectRoot, 'installed');
  const installResult = spawnSync(installer, ['/S', `/D=${installedRoot}`], {
    cwd: projectRoot,
    stdio: 'inherit',
    windowsHide: true
  });
  assert.equal(installResult.status, 0, 'The NSIS installer failed.');
  assert.equal(await exists(join(installedRoot, `${appName}.exe`)), true, 'The installer did not install the app.');
  const uninstallKey = `HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${appName}`;
  const registryResult = spawnSync('reg.exe', ['query', uninstallKey, '/v', 'EstimatedSize'], {
    encoding: 'utf8',
    windowsHide: true
  });
  assert.equal(registryResult.status, 0, 'The installer did not register EstimatedSize.');
  assert.match(registryResult.stdout, /EstimatedSize\s+REG_DWORD\s+0x[0-9a-f]+/i, 'EstimatedSize is not a DWORD.');
  const uninstallResult = spawnSync(join(installedRoot, 'Uninstall.exe'), ['/S'], {
    cwd: installedRoot,
    stdio: 'inherit',
    windowsHide: true
  });
  assert.equal(uninstallResult.status, 0, 'The NSIS uninstaller failed.');

  host = spawn(executable, [], { cwd: projectRoot, stdio: 'ignore', windowsHide: false });
  await new Promise((resolvePromise, reject) => {
    const timer = setTimeout(resolvePromise, 1000);
    host.once('error', reject);
    host.once('exit', code => {
      clearTimeout(timer);
      reject(new Error(`Tiny host exited during startup with code ${code}.`));
    });
  });
  await waitFor(() => exists(webviewRoot));

  passed = true;
  console.log('E2E passed: create, install, standalone build, NSIS install/uninstall, launch, and WebView2 initialization.');
} finally {
  if (host && host.exitCode === null) {
    spawnSync('taskkill.exe', ['/PID', String(host.pid), '/T', '/F'], {
      stdio: 'ignore',
      windowsHide: true
    });
    await waitForExit(host);
  }
  if (passed) {
    await rm(projectRoot, { recursive: true, force: true });
    await rm(dataRoot, { recursive: true, force: true });
    await rm(join(process.env.LOCALAPPDATA, appName), { recursive: true, force: true });
  } else {
    console.error(`E2E project kept for diagnosis: ${projectRoot}`);
  }
}
