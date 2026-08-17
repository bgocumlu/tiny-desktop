#!/usr/bin/env node
import { cp, mkdir, readdir, readFile, rm, stat, writeFile } from 'node:fs/promises';
import { spawn } from 'node:child_process';
import { dirname, basename, join, relative, resolve, sep } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const projectRoot = resolve(process.cwd());
const nativeBuild = join(packageRoot, 'native', 'build');
const runtime = join(packageRoot, 'runtime', 'win32-x64', 'tiny-host.exe');
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
  const defaults = { app: { name: basename(projectRoot) }, storage: { mode: 'appData' } };
  try {
    await stat(configPath);
  } catch {
    return defaults;
  }
  const loaded = await import(`${pathToFileURL(configPath).href}?v=${Date.now()}`);
  const config = loaded.default ?? loaded;
  const mode = config.storage?.mode ?? defaults.storage.mode;
  if (mode !== 'appData' && mode !== 'portable') throw new Error("tiny.config.js storage.mode must be 'appData' or 'portable'.");
  return {
    ...defaults,
    ...config,
    app: { ...defaults.app, ...config.app, name: String(config.app?.name ?? defaults.app.name) },
    storage: { mode }
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

async function buildApp() {
  await requireVite();
  await requireRuntime();
  const config = await loadConfig();
  await run(process.execPath, [vite, 'build']);
  await mkdir(release, { recursive: true });
  const filename = (config.app.name.replace(/[<>:"/\\|?*]/g, '-').trim() || 'Tiny') + '.exe';
  await rm(join(release, 'app'), { recursive: true, force: true });
  await rm(join(release, filename), { force: true });
  await bundleRuntime(runtime, join(projectRoot, 'dist'), join(release, filename), config);
  console.log(`Built release/${filename} with embedded Vite assets.`);
}

async function check() {
  await run(process.execPath, ['--check', 'cli/tiny.mjs'], { cwd: packageRoot });
  await run(process.execPath, ['--check', 'src/main.js'], { cwd: projectRoot });
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
