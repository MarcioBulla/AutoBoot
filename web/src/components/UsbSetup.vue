<script setup lang="ts">
import { computed, onUnmounted, ref, shallowRef } from 'vue';
import { AutoBootSerial } from '../services/usbSerial.js';

const state = ref('disconnected');
const error = ref('');
const notice = ref('');
const progress = ref('');
const ports = shallowRef<any[]>([]);
const selectedPort = ref('0');
const connectedPort = ref('');
const ssid = ref('');
const password = ref('');
const host = ref('mqtt.example.com');
const port = ref('18443');
const csr = ref('');
const certificate = shallowRef<File | null>(null);
const certInput = ref<HTMLInputElement | null>(null);
const certType = ref<'CLIENT' | 'CA'>('CLIENT');
const supported = AutoBootSerial.supported();
const device = new AutoBootSerial((next: string, message: string) => {
  state.value = next;
  if (message) error.value = message;
  if (next === 'disconnected' || next === 'error') {
    password.value = ''; certificate.value = null; connectedPort.value = '';
    if (certInput.value) certInput.value.value = '';
  }
});
const connected = computed(() => state.value === 'connected');
const busy = computed(() => state.value === 'busy' || state.value === 'connecting');
const enc = new TextEncoder();

async function run(task: () => Promise<void>, success = '') {
  error.value = ''; notice.value = '';
  try { await task(); notice.value = success; }
  catch (cause) {
    const err = cause as Error;
    if (err?.name === 'NotFoundError') notice.value = 'Port selection cancelled.';
    else error.value = err?.message || 'USB communication failed.';
  } finally { progress.value = ''; }
}
async function choosePort() {
  await run(async () => {
    // requestPort must run from the user's click gesture.
    const selected = await (navigator as any).serial.requestPort();
    await device.connect(selected); connectedPort.value = device.describe();
  }, 'USB port connected.');
}
async function listPorts() { await run(async () => { ports.value = await AutoBootSerial.ports(); selectedPort.value = '0'; if (!ports.value.length) notice.value = 'No previously authorized ports found.'; }); }
async function connectSaved() { await run(async () => { const selected = ports.value[Number(selectedPort.value)]; if (!selected) throw new Error('Select an authorized port.'); await device.connect(selected); connectedPort.value = device.describe(); }, 'USB port connected.'); }
async function disconnect() { await device.close(); csr.value = ''; notice.value = 'USB port disconnected.'; error.value = ''; }
async function getCsr() { await run(async () => { csr.value = await device.csr(); }, 'CSR received from the device. It contains the public key.'); }
async function copyCsr() { await run(async () => { await navigator.clipboard.writeText(csr.value); }, 'CSR copied.'); }
function downloadCsr() { const url = URL.createObjectURL(new Blob([csr.value], { type: 'application/pkcs10' })); const link = document.createElement('a'); link.href = url; link.download = 'autoboot-device.csr'; link.click(); URL.revokeObjectURL(url); }
async function saveWifi() {
  await run(async () => {
    if (!ssid.value || /\s/.test(ssid.value)) throw new Error('SSID must not contain spaces or other whitespace.');
    if (!password.value || /[\r\n\0]/.test(password.value) || /[\r\n\0]/.test(ssid.value)) throw new Error('Enter a valid SSID and password without line breaks.');
    if (enc.encode(ssid.value).length > 32 || enc.encode(password.value).length > 64) throw new Error('SSID must be at most 32 bytes and password at most 64 bytes.');
    await device.command(`WIFI ${ssid.value} ${password.value}`, 'OK wifi-saved; reboot');
  }, 'Wi-Fi settings sent and confirmed by the firmware. Reboot the device.');
  password.value = '';
}
async function saveMqtt() {
  await run(async () => {
    const value = host.value.trim(); const n = Number(port.value);
    if (!value || value.length > 253 || !/^[A-Za-z0-9.-]+$/.test(value) || value.startsWith('.') || value.endsWith('.') || value.split('.').some(x => !x || x.length > 63 || x.startsWith('-') || x.endsWith('-'))) throw new Error('Enter a valid hostname or IPv4 address.');
    if (!/^\d+$/.test(port.value) || !Number.isInteger(n) || n < 1 || n > 65535) throw new Error('Enter a port between 1 and 65535.');
    await device.command(`MQTT ${value} ${n}`, 'OK mqtt-saved');
  }, 'MQTT settings sent and confirmed by the firmware.');
}
function fileSelected(event: Event) { certificate.value = (event.target as HTMLInputElement).files?.[0] || null; }
async function upload() {
  let bytes: Uint8Array | null = null;
  await run(async () => {
    const file = certificate.value;
    if (!file || !file.size || file.size > 4096) throw new Error('Choose a certificate file of 1 to 4096 bytes.');
    bytes = new Uint8Array(await file.arrayBuffer());
    await device.upload(certType.value, bytes, (message: string) => { progress.value = message; });
  }, 'Certificate installation confirmed by the firmware.');
  bytes?.fill(0); certificate.value = null; if (certInput.value) certInput.value.value = '';
}
onUnmounted(() => { password.value = ''; certificate.value = null; csr.value = ''; void device.dispose(); });
</script>

<template>
  <div class="usb-setup">
    <header class="heading"><p class="eyebrow">DEVICE SETUP</p><h1>USB configuration</h1><p>Connect the AutoBoot device directly to this browser to provision its credentials and connection.</p></header>
    <div v-if="!supported" class="message warning" role="alert">Web Serial is unavailable. Use a browser that supports Web Serial and open this page over HTTPS or localhost.</div>
    <div v-if="error" class="message error" role="alert">{{ error }}</div>
    <div v-if="notice" class="message success" role="status">{{ notice }}</div>
    <section class="card">
      <div class="card-title"><div><p class="eyebrow">USB SERIAL</p><h2>Device connection</h2><p>Connect the MCU by USB. The connection runs at 115200 baud.</p></div><span class="status" :class="{ online: connected }">{{ state === 'busy' ? 'Operation in progress' : state }}</span></div>
      <p v-if="connectedPort" class="port-name">{{ connectedPort }}</p>
      <div class="actions">
        <button v-if="state !== 'connected' && !busy" class="primary" :disabled="!supported" @click="choosePort">Connect USB device</button>
        <button v-if="state !== 'connected' && !busy" :disabled="!supported" @click="listPorts">List authorized ports</button>
        <button v-if="state === 'connected' || state === 'busy'" @click="disconnect">Disconnect</button>
      </div>
      <div v-if="ports.length && state !== 'connected' && !busy" class="saved-ports"><label>Previously authorized ports<select v-model="selectedPort"><option v-for="(item, index) in ports" :key="index" :value="String(index)">{{ device.describe(item) }}</option></select></label><button @click="connectSaved">Connect selected</button></div>
    </section>
    <p class="hint">These forms send values over USB only. The firmware does not provide a command to read its current settings; the values below are not confirmed device settings.</p>
    <div class="grid">
      <section class="card"><p class="eyebrow">CERTIFICATE REQUEST</p><h2>Get device CSR</h2><p>Download the public certificate request for signing. The device private key stays on the MCU.</p><div class="actions"><button class="primary" :disabled="!connected" @click="getCsr">Get CSR</button><button :disabled="!csr" @click="copyCsr">Copy</button><button :disabled="!csr" @click="downloadCsr">Download .csr</button></div><textarea v-if="csr" :value="csr" readonly rows="7" aria-label="Device CSR"></textarea></section>
      <section class="card"><p class="eyebrow">WIRELESS NETWORK</p><h2>Wi-Fi</h2><p>SSID cannot contain spaces with the current firmware.</p><form @submit.prevent="saveWifi"><label>SSID<input v-model="ssid" autocomplete="off" required /></label><label>Password<input v-model="password" type="password" autocomplete="new-password" required /></label><button class="primary" :disabled="!connected">Send Wi-Fi settings</button></form></section>
      <section class="card"><p class="eyebrow">BROKER</p><h2>MQTT connection</h2><p>Suggested values are shown below; they have not been read from the device.</p><form @submit.prevent="saveMqtt"><label>Hostname<input v-model="host" required autocomplete="off" /></label><label>Port<input v-model="port" type="number" min="1" max="65535" required /></label><button class="primary" :disabled="!connected">Send MQTT settings</button></form></section>
      <section class="card"><p class="eyebrow">CERTIFICATES</p><h2>Install certificate</h2><p>Choose a PEM or CRT file up to 4096 bytes. Its bytes are sent unchanged.</p><form @submit.prevent="upload"><label>Certificate type<select v-model="certType"><option value="CLIENT">Client certificate</option><option value="CA">Broker CA</option></select></label><label>Certificate file<input ref="certInput" type="file" accept=".pem,.crt,.cer" @change="fileSelected" /></label><button class="primary" :disabled="!connected || !certificate">Install certificate</button><p v-if="progress" role="status">{{ progress }}</p></form></section>
    </div>
  </div>
</template>

<style scoped>
.usb-setup { max-width:1120px;margin:0 auto; color:#eae7f1; }
.heading { margin-bottom:24px; }.heading h1 { font-size:clamp(28px,4vw,40px);letter-spacing:-.045em;margin:0; }.heading > p:last-child,.card p,.hint { color:#9690a0;font-size:13px;line-height:1.6; }.eyebrow { color:#bb9cff!important;font-size:10px!important;letter-spacing:.18em;font-weight:750;margin:0 0 10px; }.card { border:1px solid #30293c;border-radius:16px;padding:23px;background:#15121c;min-width:0; }.card h2 { margin:0;font-size:17px; }.card-title { display:flex;justify-content:space-between;gap:12px;align-items:start; }.status { color:#b2a8bb;font-size:12px;text-transform:capitalize; }.status.online { color:#7bd9a8; }.port-name { overflow-wrap:anywhere; }.grid { display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:16px; }.hint { margin:20px 0; }.actions { display:flex;flex-wrap:wrap;gap:9px;margin-top:16px; }.saved-ports { display:flex;gap:10px;align-items:end;margin-top:16px; }.saved-ports label { flex:1; }.message { margin-bottom:14px;padding:13px 15px;border-radius:9px;font-size:13px;line-height:1.5; }.error { background:#381a24;color:#f2a8b1; }.warning { background:#322816;color:#e5c48a; }.success { background:#102b22;color:#8de1ba; } form { display:grid;gap:13px;margin-top:15px; } label { display:grid;gap:7px;color:#c6bfce;font-size:12px;font-weight:650; } input,select,textarea { width:100%;box-sizing:border-box;border:1px solid #3b3346;border-radius:9px;background:#0e0c13;color:#eee9f5;padding:11px;font:inherit;font-size:13px; } textarea { margin-top:15px;resize:vertical;font-family:monospace; } button { border:1px solid #59426e;border-radius:9px;padding:10px 15px;background:#221a2c;color:#d4bdef;font-weight:700;cursor:pointer; } button.primary { background:#9b70e0;border-color:#9b70e0;color:#160d21; } button:disabled { opacity:.45;cursor:not-allowed; } form button { justify-self:start; } @media(max-width:760px) { .grid { grid-template-columns:1fr; }.saved-ports { flex-wrap:wrap; } }
</style>
