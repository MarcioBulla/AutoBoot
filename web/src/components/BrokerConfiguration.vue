<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue';
import { startAuthentication } from '@simplewebauthn/browser';

type BrokerState = {
  config: { host: string; port: number; path: string; deviceId: string };
  csr: string;
  clientCertificatePem: string;
  brokerCaPem: string;
  hasClientCertificate: boolean;
  hasBrokerCa: boolean;
  connection: ConnectionState;
};
type Account = { totpEnabled: boolean; passkeys: Array<{ id: string }> };
type ConnectionState = { connected: boolean; connecting: boolean; handshakeSucceeded: boolean; lastHandshakeAt: string | null; lastError: string };

const broker = ref<BrokerState | null>(null);
const connection = ref<ConnectionState>({ connected: false, connecting: false, handshakeSucceeded: false, lastHandshakeAt: null, lastError: '' });
const account = ref<Account | null>(null);
const host = ref('');
const port = ref('8084');
const websocketPath = ref('/mqtt');
const deviceId = ref('AutoBoot');
const clientCertificatePem = ref('');
const brokerCaPem = ref('');
const password = ref('');
const factorCode = ref('');
const factorMethod = ref<'totp' | 'passkey'>('totp');
const busy = ref(false);
const error = ref('');
const notice = ref('');
const showCsr = ref(false);
const showRegenerate = ref(false);
let connectionTimer: ReturnType<typeof setInterval> | undefined;

const factorOptions = computed(() => [
  ...(account.value?.totpEnabled ? [{ label: 'Authenticator code', value: 'totp' as const }] : []),
  ...(account.value?.passkeys.length ? [{ label: 'Passkey', value: 'passkey' as const }] : []),
]);
const hasSecurityFactor = computed(() => factorOptions.value.length > 0);
const connectionReady = computed(() => Boolean(
  broker.value?.config.host && broker.value?.config.deviceId && broker.value?.hasClientCertificate && broker.value?.hasBrokerCa && broker.value?.csr,
));

async function api(path: string, method = 'GET', body?: Record<string, unknown>) {
  const response = await fetch(path, {
    method,
    credentials: 'same-origin',
    headers: body ? { 'Content-Type': 'application/json' } : undefined,
    body: body ? JSON.stringify(body) : undefined,
  });
  const payload = await response.json().catch(() => ({}));
  if (!response.ok) throw new Error(payload.error || 'The request could not be completed.');
  return payload;
}

async function refresh() {
  const [brokerResult, accountResult] = await Promise.all([
    api('/api/auth/broker'),
    api('/api/auth/account'),
  ]);
  broker.value = brokerResult;
  connection.value = brokerResult.connection;
  account.value = accountResult.account;
  host.value = brokerResult.config.host;
  port.value = String(brokerResult.config.port);
  websocketPath.value = brokerResult.config.path;
  deviceId.value = brokerResult.config.deviceId || 'AutoBoot';
  if (!factorOptions.value.some((factor) => factor.value === factorMethod.value)) {
    factorMethod.value = factorOptions.value[0]?.value || 'totp';
  }
}

async function refreshConnection() {
  const result = await api('/api/auth/broker/status');
  connection.value = result.connection;
}

async function run(task: () => Promise<void>, success?: string) {
  if (busy.value) return;
  busy.value = true;
  error.value = '';
  notice.value = '';
  try {
    await task();
    notice.value = success || '';
    factorCode.value = '';
    password.value = '';
  } catch (cause) {
    error.value = cause instanceof Error ? cause.message : 'The request could not be completed.';
  } finally {
    busy.value = false;
  }
}

async function currentProof(action: string) {
  if (!password.value) throw new Error('Enter your current password.');
  if (!hasSecurityFactor.value) return { currentPassword: password.value };
  if (factorMethod.value === 'totp') {
    if (!factorCode.value) throw new Error('Enter a current authenticator code.');
    return { currentPassword: password.value, factor: { method: 'totp', code: factorCode.value } };
  }
  const options = await api('/api/auth/reauth/options', 'POST', { action });
  const credential = await startAuthentication({ optionsJSON: options });
  return { currentPassword: password.value, factor: { method: 'passkey', credential } };
}

async function saveConnection() {
  await run(async () => {
    await api('/api/auth/broker/config', 'POST', { host: host.value, port: Number(port.value), path: websocketPath.value, deviceId: deviceId.value });
    await refresh();
  }, 'Broker connection details saved.');
}

async function generateCsr(replaceExisting = false) {
  if (busy.value) return;
  if (replaceExisting && !window.confirm('Generating a new CSR replaces the current client key and invalidates its signed certificate. Continue?')) return;
  await run(async () => {
    const proof = replaceExisting ? await currentProof('generate-broker-csr') : {};
    const result = await api('/api/auth/broker/csr', 'POST', { ...proof, replaceExisting });
    broker.value = { ...(broker.value as BrokerState), csr: result.csr, hasClientCertificate: false };
    showCsr.value = true;
    showRegenerate.value = false;
    await refresh();
  }, 'New certificate signing request generated. The previous signed certificate was removed.');
}

async function readPemFile(event: Event, target: 'client' | 'ca') {
  const input = event.target as HTMLInputElement;
  const file = input.files?.[0];
  if (!file) return;
  if (file.size > 100_000) {
    error.value = 'Choose a PEM certificate smaller than 100 KB.';
    input.value = '';
    return;
  }
  try {
    const bytes = new Uint8Array(await file.arrayBuffer());
    const text = new TextDecoder().decode(bytes).replace(/^\uFEFF/, '').trim();
    let pem = text;
    if (!/-----BEGIN CERTIFICATE-----/i.test(text)) {
      let der = bytes;
      const compactBase64 = text.replace(/\s/g, '');
      if (/^[A-Za-z0-9+/]+={0,2}$/.test(compactBase64) && compactBase64.length > 128) {
        try {
          const decoded = atob(compactBase64);
          const candidate = Uint8Array.from(decoded, (character) => character.charCodeAt(0));
          if (candidate[0] === 0x30) der = candidate;
        } catch {
          // Treat the file as binary DER below.
        }
      }
      let binary = '';
      for (let offset = 0; offset < der.length; offset += 0x8000) {
        binary += String.fromCharCode(...der.subarray(offset, offset + 0x8000));
      }
      const base64 = btoa(binary).match(/.{1,64}/g)?.join('\n') || '';
      pem = `-----BEGIN CERTIFICATE-----\n${base64}\n-----END CERTIFICATE-----\n`;
    }
    if (target === 'client') clientCertificatePem.value = pem;
    else brokerCaPem.value = pem;
    error.value = '';
  } catch {
    error.value = 'Could not read that file.';
  } finally {
    input.value = '';
  }
}

async function saveClientCertificate() {
  await run(async () => {
    await api('/api/auth/broker/client-certificate', 'POST', { pem: clientCertificatePem.value });
    clientCertificatePem.value = '';
    await refresh();
  }, 'Signed client certificate saved and matched to the generated CSR.');
}

async function saveBrokerCa() {
  await run(async () => {
    await api('/api/auth/broker/ca', 'POST', { pem: brokerCaPem.value });
    brokerCaPem.value = '';
    await refresh();
  }, 'Broker CA saved.');
}

async function connectToBroker() {
  await run(async () => {
    if (!host.value || host.value !== broker.value?.config.host || Number(port.value) !== broker.value?.config.port || websocketPath.value !== broker.value?.config.path) {
      throw new Error('Save the broker host, port, and WebSocket path first.');
    }
    const result = await api('/api/auth/broker/connect', 'POST', {});
    connection.value = result.connection;
  }, 'Connection attempt started. The status will update automatically.');
}

async function disconnectFromBroker() {
  await run(async () => {
    const result = await api('/api/auth/broker/disconnect', 'POST', {});
    connection.value = result.connection;
  }, 'Disconnected from the broker.');
}

function downloadText(filename: string, content: string) {
  const blob = new Blob([content], { type: 'application/x-pem-file' });
  const link = document.createElement('a');
  link.href = URL.createObjectURL(blob);
  link.download = filename;
  link.click();
  window.setTimeout(() => URL.revokeObjectURL(link.href), 1000);
}

async function copyCsr() {
  if (!broker.value?.csr) return;
  try {
    await navigator.clipboard.writeText(broker.value.csr);
    notice.value = 'CSR copied to clipboard.';
    error.value = '';
  } catch {
    error.value = 'Clipboard access is unavailable. Select and copy the CSR from the text box.';
  }
}

onMounted(async () => {
  try {
    await refresh();
    connectionTimer = setInterval(() => refreshConnection().catch(() => {}), 3000);
  } catch (cause) {
    error.value = cause instanceof Error ? cause.message : 'Could not load broker settings.';
  }
});
onUnmounted(() => { if (connectionTimer) clearInterval(connectionTimer); });
</script>

<template>
  <section class="broker-page" aria-labelledby="broker-title">
    <header class="section-heading">
      <div>
        <p class="eyebrow">BROKER ACCESS</p>
        <h2 id="broker-title">WebSocket broker</h2>
        <p class="muted">Configure a secure MQTT over WebSocket connection using mutual TLS.</p>
      </div>
      <span class="secure-pill"><i></i> WSS · mTLS</span>
    </header>

    <p v-if="error" class="feedback error" role="alert">{{ error }}</p>
    <p v-if="notice" class="feedback success" role="status">{{ notice }}</p>
    <div v-if="!broker" class="loading-panel">Loading broker settings…</div>
    <template v-else>
      <article class="connection-status-card" :class="{ online: connection.connected, connecting: connection.connecting }" aria-live="polite">
        <span class="connection-light" :class="{ online: connection.connected, connecting: connection.connecting }"></span>
        <div class="connection-copy">
          <strong>MQTT {{ connection.connecting ? 'Connecting' : connection.connected ? 'Connected' : 'Disconnected' }}</strong>
          <span>Last MQTT/TLS handshake: {{ connection.handshakeSucceeded ? `Succeeded${connection.lastHandshakeAt ? ` · ${new Date(connection.lastHandshakeAt).toLocaleString()}` : ''}` : connection.connecting ? 'In progress' : 'Not confirmed' }}</span>
          <span v-if="connection.lastError" class="connection-error">{{ connection.lastError }}</span>
        </div>
        <div class="actions status-actions">
          <button class="primary-button" type="button" :disabled="busy || connection.connected || connection.connecting || !connectionReady" @click="connectToBroker">{{ connection.connecting ? 'Connecting…' : 'Connect' }}</button>
          <button class="secondary-button" type="button" :disabled="busy || (!connection.connected && !connection.connecting)" @click="disconnectFromBroker">Disconnect</button>
        </div>
      </article>

      <article class="broker-card">
        <div class="card-heading"><span class="step">01</span><div><h3>Connection address</h3><p>Enter the broker host, secure WebSocket port, and WebSocket endpoint.</p></div></div>
        <form class="connection-form" @submit.prevent="saveConnection">
          <label class="host-field">Host or IP address<input v-model="host" type="text" autocomplete="url" placeholder="broker.example.com" required maxlength="253" /></label>
          <label>Port<input v-model="port" type="number" min="1" max="65535" placeholder="8084" required /></label>
          <label>WebSocket path<input v-model="websocketPath" type="text" placeholder="/mqtt" required maxlength="200" /></label>
          <label>Device ID<input v-model="deviceId" type="text" placeholder="AutoBoot" required maxlength="128" pattern="[^/# +]+" title="Use one MQTT topic segment without spaces, slash, +, or #" /></label>
          <div class="actions"><button class="primary-button" :disabled="busy">Save connection</button></div>
        </form>
      </article>

      <article class="broker-card">
        <div class="card-heading"><span class="step">02</span><div><h3>Client certificate request</h3><p>Generate a private key and CSR for your broker administrator to sign. The request uses the saved Device ID as its identity, so the site and device may share broker permissions. The private key stays protected on this server.</p></div></div>
        <div class="status-row"><span :class="['status-chip', broker.csr ? 'ready' : 'pending']"><i></i>{{ broker.csr ? 'CSR generated' : 'CSR not generated' }}</span><span :class="['status-chip', broker.hasClientCertificate ? 'ready' : 'pending']"><i></i>{{ broker.hasClientCertificate ? 'Signed certificate installed' : 'Signed certificate needed' }}</span></div>
        <div class="actions csr-actions">
          <button v-if="!broker.csr" class="primary-button" :disabled="busy" @click="generateCsr()">Generate CSR</button>
          <button v-else class="primary-button" :disabled="busy" @click="showCsr = !showCsr">{{ showCsr ? 'Hide CSR' : 'View CSR' }}</button>
          <button v-if="broker.csr" class="secondary-button" :disabled="busy" @click="downloadText('autobootweb-client.csr.pem', broker.csr)">Download CSR</button>
          <button v-if="broker.csr" class="quiet-button" :disabled="busy" @click="copyCsr">Copy CSR</button>
          <button v-if="broker.csr" class="quiet-button danger" :disabled="busy" @click="showRegenerate = !showRegenerate">Generate new CSR</button>
        </div>
        <div v-if="showRegenerate" class="proof-panel">
          <p>Replacing the CSR deletes the installed client certificate. Verify your identity to continue.</p>
          <label>Current password<input v-model="password" type="password" autocomplete="current-password" required /></label>
          <div v-if="hasSecurityFactor" class="factor-panel"><p>Also verify with one registered factor</p><div class="factor-options"><label v-for="factor in factorOptions" :key="factor.value"><input v-model="factorMethod" type="radio" :value="factor.value" />{{ factor.label }}</label></div><label v-if="factorMethod === 'totp'">Authenticator code<input v-model="factorCode" type="text" inputmode="numeric" autocomplete="one-time-code" pattern="[0-9]{6}" maxlength="6" required /></label></div>
          <div class="actions"><button class="primary-button" :disabled="busy" @click="generateCsr(true)">Confirm and replace CSR</button><button class="quiet-button" type="button" :disabled="busy" @click="showRegenerate = false">Cancel</button></div>
        </div>
        <textarea v-if="broker.csr && showCsr" class="pem-view" :value="broker.csr" readonly aria-label="Generated certificate signing request"></textarea>
      </article>

      <article class="broker-card">
        <div class="card-heading"><span class="step">03</span><div><h3>Signed client certificate</h3><p>After signing the CSR, paste or upload the resulting client certificate PEM.</p></div></div>
        <label class="file-picker">Upload signed certificate<input type="file" accept=".crt,.cer,.pem,.der,application/x-pem-file,application/pkix-cert" @change="readPemFile($event, 'client')" /></label>
        <label class="pem-label">Or paste the signed certificate<textarea v-model="clientCertificatePem" class="pem-input" rows="6" placeholder="-----BEGIN CERTIFICATE-----"></textarea></label>
        <div class="actions"><button class="primary-button" :disabled="busy || !clientCertificatePem.trim() || !broker.csr" @click="saveClientCertificate">Save signed certificate</button><button v-if="broker.hasClientCertificate" class="quiet-button" :disabled="busy" @click="downloadText('autobootweb-client-cert.pem', broker.clientCertificatePem)">Download installed CRT</button><span v-if="!broker.csr" class="inline-help">Generate a CSR first.</span></div>
      </article>

      <article class="broker-card">
        <div class="card-heading"><span class="step">04</span><div><h3>Broker CA certificate</h3><p>Add the CA certificate used to verify the broker during the WSS handshake.</p></div></div>
        <label class="file-picker">Upload broker CA<input type="file" accept=".crt,.cer,.pem,.der,application/x-pem-file,application/pkix-cert" @change="readPemFile($event, 'ca')" /></label>
        <label class="pem-label">Or paste the broker CA PEM<textarea v-model="brokerCaPem" class="pem-input" rows="6" placeholder="-----BEGIN CERTIFICATE-----"></textarea></label>
        <div class="actions"><button class="primary-button" :disabled="busy || !brokerCaPem.trim()" @click="saveBrokerCa">Save broker CA</button><button v-if="broker.hasBrokerCa" class="quiet-button" :disabled="busy" @click="downloadText('autobootweb-broker-ca.pem', broker.brokerCaPem)">Download installed CA</button><span :class="['status-chip', broker.hasBrokerCa ? 'ready' : 'pending']"><i></i>{{ broker.hasBrokerCa ? 'CA installed' : 'CA needed' }}</span></div>
      </article>

    </template>
  </section>
</template>

<style scoped>
.broker-page { max-width: 1040px; margin: 0 auto; color: #e8e5f0; }
.section-heading { display:flex;align-items:flex-end;justify-content:space-between;gap:20px;margin:8px 0 25px; }
.eyebrow { margin:0 0 9px;color:#bd9cff;font-size:10px;font-weight:780;letter-spacing:.18em; }
h2 { margin:0;color:#efedf3;font-size:clamp(25px,4vw,34px);letter-spacing:-.04em; }
.muted,.card-heading p { color:#928d9d; }
.muted { margin:8px 0 0;font-size:14px; }
.secure-pill { display:flex;align-items:center;gap:8px;color:#a8a0b4;font-size:12px; }
.secure-pill i,.status-chip i { width:8px;height:8px;border-radius:50%;background:#55d6a3;box-shadow:0 0 12px #55d6a377; }
.connection-status-card { display:flex;align-items:center;gap:14px;margin:14px 0 18px;padding:17px 20px;border:1px solid #40323f;border-radius:15px;background:linear-gradient(110deg,#1a141d,#121018); }
.connection-status-card.online { border-color:#285642;background:linear-gradient(110deg,#12231e,#121018); }
.connection-light { flex:0 0 11px;width:11px;height:11px;border-radius:50%;background:#77717f; }
.connection-light.online { background:#58d4a0;box-shadow:0 0 14px #58d4a099; }
.connection-light.connecting { background:#e5bd6b;box-shadow:0 0 14px #e5bd6b88;animation:pulse 1.2s infinite; }
.connection-copy { display:flex;flex:1;flex-direction:column;gap:4px;min-width:0; }
.connection-copy strong { color:#e9e4ee;font-size:14px; }
.connection-copy span { color:#9992a1;font-size:11px; }
.connection-copy .connection-error { color:#ffaaaa; }
.status-actions { margin-left:auto; }
@keyframes pulse { 50% { opacity:.4; } }
.broker-card,.loading-panel { margin:14px 0;padding:23px;border:1px solid #2d2838;border-radius:17px;background:linear-gradient(145deg,#16131f,#121018); }
.card-heading { display:flex;align-items:flex-start;gap:13px;margin-bottom:19px; }
.card-heading .step { display:grid;place-items:center;flex:0 0 37px;height:37px;border:1px solid #51416e;border-radius:11px;color:#c4aaff;font-size:11px;font-weight:750;letter-spacing:.04em; }
h3 { margin:0;color:#e8e5f0;font-size:16px; }
.card-heading p { margin:5px 0 0;font-size:13px;line-height:1.55; }
.connection-form { display:grid;grid-template-columns:minmax(200px,1fr) 120px minmax(140px,.7fr);gap:14px;align-items:end; }
label { display:flex;flex-direction:column;gap:8px;color:#c6c0d0;font-size:12px;font-weight:620; }
input:not([type=radio]):not([type=file]),.pem-input,.pem-view { width:100%;min-height:43px;padding:0 12px;border:1px solid #373141;border-radius:10px;outline:0;background:#0d0b12;color:#eeeaf5;font:inherit;font-size:14px; }
input:focus,.pem-input:focus { border-color:#9871e8;box-shadow:0 0 0 3px #9871e81c; }
.actions { display:flex;align-items:center;flex-wrap:wrap;gap:9px; }
.connection-form .actions { grid-column:1/-1;margin-top:3px; }
button { min-height:40px;padding:0 15px;border-radius:10px;color:#eeeaf5;cursor:pointer;font-size:12px;font-weight:680; }
button:disabled { opacity:.5;cursor:not-allowed; }
.primary-button { border:0;background:linear-gradient(100deg,#8753db,#b177ff);color:#100b19; }
.secondary-button { border:1px solid #51416e;background:#211a2a;color:#d2c2e8; }
.quiet-button { border:1px solid #393343;background:#17141e;color:#b9b2c3; }
.danger { color:#ffaaaa; }
.status-row { display:flex;flex-wrap:wrap;gap:8px;margin-bottom:17px; }
.status-chip { display:inline-flex;align-items:center;gap:7px;width:max-content;padding:7px 10px;border:1px solid #35303d;border-radius:999px;color:#9992a1;font-size:10px; }
.status-chip i { width:6px;height:6px;background:#706a77;box-shadow:none; }
.status-chip.ready { border-color:#285642;color:#9cdbbd; }
.status-chip.ready i { background:#58d4a0;box-shadow:0 0 10px #58d4a077; }
.csr-actions { margin-bottom:12px; }
.pem-view,.pem-input { min-height:150px!important;padding:12px!important;resize:vertical;white-space:pre;overflow:auto;font:12px/1.55 ui-monospace,SFMono-Regular,Menlo,monospace!important; }
.file-picker { width:max-content;max-width:100%;margin-bottom:14px; }
.file-picker input { max-width:100%;color:#968e9f;font-size:11px; }
.pem-label { margin-bottom:13px; }
.feedback { margin:14px 0 0;padding:12px 14px;border-radius:10px;font-size:12px;line-height:1.5; }
.feedback.error { border:1px solid #744144;background:#2b171b;color:#ffb2b3; }
.feedback.success { border:1px solid #285b4a;background:#10231e;color:#8fe0bd; }
.proof-panel { display:grid;max-width:520px;gap:12px;margin-top:15px;padding:16px;border:1px solid #47354c;border-radius:12px;background:#1a141f; }
.proof-panel p,.inline-help { margin:0;color:#a29aaa;font-size:12px;line-height:1.55; }
.factor-panel { display:grid;gap:9px; }
.factor-panel > p { margin:0;color:#a49bad;font-size:11px; }
.factor-options { display:flex;flex-wrap:wrap;gap:8px; }
.factor-options label { display:flex;flex-direction:row;align-items:center;gap:7px;padding:8px 10px;border:1px solid #393343;border-radius:9px;background:#100e15;color:#c3bdcc;font-size:11px;cursor:pointer; }
.factor-options input { width:14px;height:14px;margin:0;accent-color:#a77bec; }
.loading-panel { color:#918a99;font-size:13px; }
@media(max-width:760px) { .broker-card { padding:18px; }.connection-form { grid-template-columns:1fr 1fr; }.host-field { grid-column:1/-1; }.connection-form .actions { grid-column:1/-1; }.connection-status-card { align-items:flex-start;flex-wrap:wrap;padding:16px; }.connection-copy { flex-basis:calc(100% - 30px); }.status-actions { width:100%;margin-left:25px; } }
</style>
