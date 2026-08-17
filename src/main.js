import './style.css';

const status = document.querySelector('#status');
const output = document.querySelector('#output');
const save = document.querySelector('#save');
const load = document.querySelector('#load');

const dataFile = async () => `${await window.native.app.getDataPath()}\\demo.json`;

async function showLocation() {
  if (!window.native) {
    status.textContent = 'Native bridge unavailable (open this page in Tiny).';
    return;
  }
  status.textContent = `Native bridge ready. Data: ${await window.native.app.getDataPath()}`;
}

save.addEventListener('click', async () => {
  const path = await dataFile();
  await window.native.fs.writeText(path, JSON.stringify({ savedAt: new Date().toISOString() }, null, 2));
  output.textContent = `Saved ${path}`;
});

load.addEventListener('click', async () => {
  const path = await dataFile();
  output.textContent = await window.native.fs.readText(path);
});

showLocation().catch((error) => {
  status.textContent = error.message;
});
