import './style.css';

const status = document.querySelector('#status');
const output = document.querySelector('#output');
const actions = document.querySelectorAll('.toolbar-button');
const store = 'main';
const tiny = window.tiny;

if (!tiny) {
  status.textContent = 'Preview only — run Tiny.';
  actions.forEach((action) => { action.disabled = true; });
} else {
  status.textContent = `Data: ${await tiny.app.getDataPath()}`;
}

document.querySelector('#save').addEventListener('click', async () => {
  if (!tiny) return;
  await tiny.data.write(store, { savedAt: new Date().toISOString() });
  output.textContent = 'Saved main.json';
});

document.querySelector('#load').addEventListener('click', async () => {
  if (!tiny) return;
  const value = await tiny.data.read(store);
  output.textContent = value ? JSON.stringify(value, null, 2) : 'No data saved yet.';
});
