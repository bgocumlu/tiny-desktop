#!/usr/bin/env node
import { mkdir, readdir, readFile, writeFile } from 'node:fs/promises';
import { spawn } from 'node:child_process';
import { basename, dirname, extname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const templateRoot = join(packageRoot, 'template');
const usage = 'Usage: npx create-tiny-desktop <directory> [--no-install]';

function slug(value) {
  return value.toLowerCase().replace(/[^a-z0-9._-]+/g, '-').replace(/^-+|-+$/g, '') || 'tiny-app';
}

function runInstall(target) {
  return new Promise((resolvePromise, reject) => {
    const npmCli = process.env.npm_execpath;
    const command = npmCli ? process.execPath : process.platform === 'win32' ? 'npm.cmd' : 'npm';
    const args = npmCli ? [npmCli, 'install'] : ['install'];
    const child = spawn(command, args, { cwd: target, stdio: 'inherit' });
    child.on('error', reject);
    child.on('exit', code => code ? reject(new Error(`npm install exited with ${code}`)) : resolvePromise());
  });
}

async function copyTemplate(sourceRoot, target, appName) {
  const binaryExtensions = new Set(['.ico', '.icns', '.png', '.jpg', '.jpeg', '.gif', '.webp']);
  const entries = await readdir(sourceRoot, { withFileTypes: true });
  for (const entry of entries) {
    if (entry.name === 'package.json') continue;
    const source = join(sourceRoot, entry.name);
    const destination = join(target, entry.name === 'gitignore' ? '.gitignore' : entry.name);
    if (entry.isDirectory()) {
      await mkdir(destination, { recursive: true });
      await copyTemplate(source, destination, appName);
    } else {
      const contents = await readFile(source);
      const output = binaryExtensions.has(extname(entry.name).toLowerCase())
        ? contents
        : contents.toString('utf8').replaceAll('__TINY_APP_NAME__', appName);
      await writeFile(destination, output);
    }
  }
}

async function create() {
  const args = process.argv.slice(2);
  if (args.includes('--help') || args.includes('-h')) {
    console.log(usage);
    return;
  }
  const options = args.filter(arg => arg.startsWith('-'));
  const directories = args.filter(arg => !arg.startsWith('-'));
  const unknownOption = options.find(option => option !== '--no-install');
  if (unknownOption) throw new Error(`Unknown option: ${unknownOption}\n\n${usage}`);
  if (directories.length !== 1) throw new Error(`A target directory is required.\n\n${usage}`);
  const noInstall = args.includes('--no-install');
  const targetArg = directories[0];
  const target = resolve(process.cwd(), targetArg);
  const appName = basename(target);
  const packageName = slug(appName);
  const runtimePackage = process.env.TINY_RUNTIME_PACKAGE ?? '^0.1.0';

  try {
    if ((await readdir(target)).length) throw new Error(`Directory is not empty: ${target}`);
  } catch (error) {
    if (error.code !== 'ENOENT') throw error;
  }
  await mkdir(target, { recursive: true });
  await copyTemplate(templateRoot, target, appName);
  await writeFile(join(target, 'package.json'), JSON.stringify({
    name: packageName,
    private: true,
    type: 'module',
    scripts: { dev: 'tiny dev', build: 'tiny build' },
    devDependencies: { 'tiny-desktop': runtimePackage, vite: '^8.2.1' }
  }, null, 2) + '\n');

  if (!noInstall) await runInstall(target);
  console.log(`Created ${appName}.`);
  if (noInstall) console.log(`Next: cd ${targetArg} && npm install`);
  else console.log(`Next: cd ${targetArg} && npm run dev`);
}

create().catch(error => {
  console.error(`Error: ${error.message}`);
  process.exitCode = 1;
});
