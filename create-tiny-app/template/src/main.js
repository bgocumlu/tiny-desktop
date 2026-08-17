import './style.css';

const status = document.querySelector('#status');
const output = document.querySelector('#output');
const store = 'main';

if (!window.tiny) {
  status.textContent = 'Open this project with Tiny.';
} else {
  status.textContent = `Data: ${await window.tiny.app.getDataPath()}`;
}

document.querySelector('#save').addEventListener('click', async () => {
  await window.tiny.data.write(store, { savedAt: new Date().toISOString() });
  output.textContent = 'Saved main.json';
});

document.querySelector('#load').addEventListener('click', async () => {
  const value = await window.tiny.data.read(store);
  output.textContent = value ? JSON.stringify(value, null, 2) : 'No data saved yet.';
});
