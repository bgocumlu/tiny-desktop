import { cp, mkdir, stat } from 'node:fs/promises';
import { spawn } from 'node:child_process';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const nativeBuild = join(root, 'native', 'build');
const release = join(root, 'release');
const vite = join(root, 'node_modules', 'vite', 'bin', 'vite.js');

function run(command, args, options = {}) {
  return new Promise((resolvePromise, reject) => {
    const child = spawn(command, args, {
      cwd: root,
      stdio: 'inherit',
      ...options
    });
    child.on('error', reject);
    child.on('exit', (code, signal) => {
      if (signal) reject(new Error(`${command} stopped with ${signal}`));
      else if (code) reject(new Error(`${command} exited with ${code}`));
      else resolvePromise();
    });
  });
}

async function buildNative() {
  await run('cmake', ['-S', 'native', '-B', 'native/build', '-A', 'x64']);
  await run('cmake', ['--build', 'native/build', '--config', 'Release']);
  return join(nativeBuild, 'Release', 'tiny-host.exe');
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

async function startDev() {
  await stat(vite).catch(() => {
    throw new Error('Vite is not installed. Run npm install first.');
  });

  const host = await buildNative();
  const url = 'http://127.0.0.1:5173/';
  const viteProcess = spawn(process.execPath, [vite, '--host', '127.0.0.1', '--port', '5173', '--strictPort'], {
    cwd: root,
    stdio: 'inherit',
    windowsHide: true
  });

  let runtime;
  const stop = () => {
    runtime?.kill();
    viteProcess.kill();
  };
  process.once('SIGINT', stop);
  process.once('SIGTERM', stop);

  try {
    await waitForVite(url);
    runtime = spawn(host, ['--dev', url, '--devtools'], { cwd: root, stdio: 'ignore', windowsHide: true });
    await new Promise((resolvePromise, reject) => {
      runtime.once('error', reject);
      runtime.once('exit', (code) => code ? reject(new Error(`tiny-host exited with ${code}`)) : resolvePromise());
    });
  } finally {
    stop();
    process.removeListener('SIGINT', stop);
    process.removeListener('SIGTERM', stop);
  }
}

async function buildApp() {
  await stat(vite).catch(() => {
    throw new Error('Vite is not installed. Run npm install first.');
  });
  await run(process.execPath, [vite, 'build']);
  const host = await buildNative();
  await mkdir(release, { recursive: true });
  await cp(host, join(release, 'Tiny.exe')); 
  await cp(join(root, 'dist'), join(release, 'app'), { recursive: true });
  console.log('Built release/Tiny.exe with release/app assets.');
}

async function check() {
  await run(process.execPath, ['--check', 'cli/tiny.mjs']);
  await run(process.execPath, ['--check', 'src/main.js']);
  await buildNative();
  console.log('Checks passed.');
}

const command = process.argv[2] ?? 'help';
try {
  if (command === 'dev') await startDev();
  else if (command === 'build') await buildApp();
  else if (command === 'check') await check();
  else console.log('Usage: npm run dev | npm run build | npm run check');
} catch (error) {
  console.error(error.message);
  process.exitCode = 1;
}
