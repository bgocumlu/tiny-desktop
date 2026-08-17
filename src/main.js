import './style.css';

const status = document.querySelector('#status');
const output = document.querySelector('#output');
const save = document.querySelector('#save');
const load = document.querySelector('#load');

const dataStore = 'demo';

async function showLocation() {
  if (!window.tiny) {
    status.textContent = 'Native bridge unavailable (open this page in Tiny).';
    return;
  }
  status.textContent = `Native bridge ready. Data: ${await window.tiny.app.getDataPath()}`;
}

save.addEventListener('click', async () => {
  await window.tiny.data.write(dataStore, { savedAt: new Date().toISOString() });
  output.textContent = `Saved ${dataStore}.json`;
});

load.addEventListener('click', async () => {
  const value = await window.tiny.data.read(dataStore);
  output.textContent = value ? JSON.stringify(value, null, 2) : 'No data saved yet.';
});

showLocation().catch((error) => {
  status.textContent = error.message;
});
