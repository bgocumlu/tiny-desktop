import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import { mkdtemp, readFile, rm, stat } from 'node:fs/promises';
import { basename, dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

if (process.platform !== 'win32' || process.arch !== 'x64') {
  throw new Error('Tiny Windows e2e requires Windows x64.');
}

const repoRoot = resolve(fileURLToPath(new URL('..', import.meta.url)));
const npmCli = process.env.npm_execpath;
if (!npmCli) throw new Error('Run the e2e test with npm test.');
const projectRoot = await mkdtemp(join(repoRoot, '.tmp-e2e-'));
const appName = basename(projectRoot);
const dataRoot = join(process.env.APPDATA, appName);
const webviewRoot = join(process.env.LOCALAPPDATA, appName, 'WebView2');
let host;
let passed = false;
const previousRuntimePackage = process.env.TINY_RUNTIME_PACKAGE;

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

  const runtimePath = resolve(repoRoot, '.local-packages', 'tiny-desktop-mvp-0.1.0.tgz').replaceAll('\\', '/');
  const env = { ...process.env, TINY_RUNTIME_PACKAGE: `file:${runtimePath}` };
  const createPackage = './.local-packages/create-tiny-app-0.1.0.tgz';
  run(['exec', '--yes', `--package=${createPackage}`, '--', 'create-tiny-app', projectRoot], repoRoot, env);

  for (const path of ['package.json', '.gitignore', 'index.html', 'src/main.js', 'src/style.css', 'tiny.config.js', 'assets/icon.ico']) {
    assert.equal(await exists(join(projectRoot, path)), true, `Missing generated file: ${path}`);
  }

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
  console.log('E2E passed: create, install, build, bundle, launch, and WebView2 initialization.');
} finally {
  if (host && host.exitCode === null) {
    spawnSync('taskkill.exe', ['/PID', String(host.pid), '/T', '/F'], {
      stdio: 'ignore',
      windowsHide: true
    });
    await waitForExit(host);
  }
  if (previousRuntimePackage === undefined) delete process.env.TINY_RUNTIME_PACKAGE;
  else process.env.TINY_RUNTIME_PACKAGE = previousRuntimePackage;
  if (passed) {
    await rm(projectRoot, { recursive: true, force: true });
    await rm(dataRoot, { recursive: true, force: true });
    await rm(join(process.env.LOCALAPPDATA, appName), { recursive: true, force: true });
  } else {
    console.error(`E2E project kept for diagnosis: ${projectRoot}`);
  }
}
