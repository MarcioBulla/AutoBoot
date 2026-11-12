<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue';

type Connection = { connected: boolean; connecting: boolean; protocolVersion: number | null; subscriptionsReady: boolean; lastError: string };
type DeviceState = {
  availability: 'online' | 'offline' | 'unknown';
  state: 'ON' | 'OFF' | 'unknown';
  updatedAt: string | null;
  lastCommand: { commandId: string; command: string; topic: string; status: string; sentAt: string; completedAt?: string } | null;
};

const emit = defineEmits<{ (event: 'open-settings'): void }>();
const connection = ref<Connection>({ connected: false, connecting: false, protocolVersion: null, subscriptionsReady: false, lastError: '' });
const device = ref<DeviceState>({ availability: 'unknown', state: 'unknown', updatedAt: null, lastCommand: null });
const deviceId = ref('AutoBoot');
const loading = ref(true);
const busyCommand = ref('');
const error = ref('');
const notice = ref('');
let timer: ReturnType<typeof setInterval> | undefined;

const canRequestState = computed(() => connection.value.connected && Boolean(deviceId.value));
const canControl = computed(() => canRequestState.value && device.value.availability === 'online');

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
  const [broker, result] = await Promise.all([api('/api/auth/broker'), api('/api/auth/broker/status')]);
  deviceId.value = broker.config.deviceId || 'AutoBoot';
  connection.value = result.connection;
  device.value = result.device;
}

async function sendCommand(command: 'get' | 'start' | 'restart') {
  if (busyCommand.value) return;
  busyCommand.value = command;
  error.value = '';
  notice.value = '';
  try {
    const result = await api('/api/auth/broker/command', 'POST', { command });
    device.value.lastCommand = result.command;
    notice.value = result.command.status === 'NO_SUBSCRIBERS'
      ? `The broker reports no subscribers for ${result.command.topic}.`
      : `${result.command.topic} acknowledged by the broker.`;
  } catch (cause) {
    error.value = cause instanceof Error ? cause.message : 'The command could not be sent.';
  } finally {
    busyCommand.value = '';
  }
}

onMounted(async () => {
  try {
    await refresh();
    timer = setInterval(() => refresh().catch(() => {}), 2000);
  } catch (cause) {
    error.value = cause instanceof Error ? cause.message : 'Could not load device status.';
  } finally {
    loading.value = false;
  }
});
onUnmounted(() => { if (timer) clearInterval(timer); });
</script>

<template>
  <section class="control-page" aria-labelledby="control-title">
    <header class="page-heading">
      <div><p class="eyebrow">WORKSPACE</p><h1 id="control-title">Device control</h1><p>Monitor the PC and send secure power commands over MQTT.</p></div>
      <span class="device-id">{{ deviceId ? `Device · ${deviceId}` : 'Device ID not configured' }}</span>
    </header>

    <article class="connection-card" :class="{ online: connection.connected, connecting: connection.connecting }" aria-live="polite">
      <span class="connection-dot" :class="{ online: connection.connected, connecting: connection.connecting }"></span>
      <div class="connection-copy"><strong>MQTT {{ connection.connecting ? 'Connecting' : connection.connected ? 'Connected' : 'Disconnected' }}</strong><span>{{ connection.lastError || (connection.connected ? `MQTT ${connection.protocolVersion === 5 ? '5' : '3.1.1'} · ${connection.subscriptionsReady ? 'subscribed to device topics' : 'waiting for topic subscriptions'}.` : 'Connect to the broker in Settings to control the device.') }}</span></div>
      <button class="quiet-button" type="button" @click="emit('open-settings')">MQTT settings</button>
    </article>

    <div v-if="loading" class="loading-card">Loading device status…</div>
    <template v-else>
      <div class="status-grid">
        <article class="status-card">
          <div class="status-title"><span class="status-icon">◉</span><div><p>AVAILABILITY</p><h2>Device connection</h2></div></div>
          <div class="status-value" :class="device.availability"><i></i>{{ device.availability === 'unknown' ? 'Unknown' : device.availability }}</div>
          <p class="status-help">{{ device.availability === 'offline' ? 'The broker reports that the device disconnected.' : device.availability === 'online' ? 'The device is connected to the broker.' : 'Waiting for a retained availability message.' }}</p>
          <code>{{ deviceId || '{Device_ID}' }}/availability</code>
        </article>

        <article class="status-card state-card">
          <div class="status-title"><span class="status-icon">⏻</span><div><p>POWER STATE</p><h2>PC state</h2></div></div>
          <div class="state-value" :class="device.state">{{ device.state }}</div>
          <p class="status-help">Reported by the device and retained by the broker.</p>
          <code>{{ deviceId || '{Device_ID}' }}/state</code>
        </article>
      </div>

      <article class="commands-card">
        <div class="commands-heading"><div><p class="eyebrow">ACTIONS</p><h2>Control the PC</h2><p>Commands are sent with a unique ID and matched to the device result.</p></div><span v-if="!canControl" class="disabled-hint">{{ !connection.connected ? 'Waiting for broker' : device.availability === 'offline' ? 'Device is offline' : 'Waiting for device availability' }}</span></div>
        <div class="command-buttons">
          <button class="reload-button" type="button" :disabled="!canRequestState || Boolean(busyCommand)" @click="sendCommand('get')"><span>{{ busyCommand === 'get' ? '…' : '↻' }}</span> Reload state</button>
          <button class="start-button" type="button" :disabled="!canControl || Boolean(busyCommand)" @click="sendCommand('start')">Start</button>
          <button class="restart-button" type="button" :disabled="!canControl || Boolean(busyCommand)" @click="sendCommand('restart')">Restart</button>
        </div>
        <div v-if="device.lastCommand" class="command-result" role="status"><span>Last request</span><strong>{{ device.lastCommand.command }} · {{ device.lastCommand.status }}</strong><code>{{ device.lastCommand.topic }}</code><code>{{ device.lastCommand.commandId }}</code><time>{{ new Date(device.lastCommand.completedAt || device.lastCommand.sentAt).toLocaleString() }}</time></div>
      </article>
    </template>
    <p v-if="error" class="feedback error" role="alert">{{ error }}</p>
    <p v-if="notice" class="feedback success" role="status">{{ notice }}</p>
  </section>
</template>

<style scoped>
.control-page { max-width:1080px;margin:0 auto;color:#eae7f1; }
.page-heading { display:flex;align-items:flex-end;justify-content:space-between;gap:20px;margin:9px 0 25px; }
.eyebrow { margin:0 0 9px;color:#bd9cff;font-size:10px;font-weight:780;letter-spacing:.18em; }
h1 { margin:0;color:#efedf3;font-size:clamp(27px,4vw,36px);letter-spacing:-.045em; }
.page-heading > div > p:last-child { margin:8px 0 0;color:#918b9a;font-size:13px; }
.device-id { color:#aaa2b3;font-size:12px; }
.connection-card,.status-card,.commands-card,.loading-card { border:1px solid #2d2838;border-radius:16px;background:linear-gradient(145deg,#17141f,#121018); }
.connection-card { display:flex;align-items:center;gap:13px;margin-bottom:17px;padding:16px 19px;border-color:#49364f; }
.connection-card.online { border-color:#285642;background:linear-gradient(110deg,#12231e,#121018); }
.connection-card.connecting { border-color:#66522d; }
.connection-dot { width:11px;height:11px;flex:0 0 11px;border-radius:50%;background:#77717f; }
.connection-dot.online { background:#58d4a0;box-shadow:0 0 14px #58d4a099; }
.connection-dot.connecting { background:#e5bd6b;animation:pulse 1s infinite; }
.connection-copy { display:flex;flex:1;flex-direction:column;gap:4px;min-width:0; }
.connection-copy strong { color:#e9e4ee;font-size:14px; }
.connection-copy span { color:#9992a1;font-size:11px; }
.status-grid { display:grid;grid-template-columns:1fr 1fr;gap:15px; }
.status-card { min-height:226px;padding:22px; }
.status-title { display:flex;align-items:center;gap:12px; }
.status-icon { display:grid;place-items:center;width:38px;height:38px;border:1px solid #51416e;border-radius:12px;color:#c4aaff;font-size:19px; }
.status-title p { margin:0 0 4px;color:#81798d;font-size:9px;font-weight:750;letter-spacing:.16em; }
h2 { margin:0;color:#e9e5ef;font-size:15px; }
.status-value { display:flex;align-items:center;gap:9px;margin:22px 0 4px;color:#bcb5c8;font-size:23px;font-weight:760;text-transform:capitalize; }
.status-value i { width:10px;height:10px;border-radius:50%;background:#77717f; }
.status-value.online i { background:#58d4a0;box-shadow:0 0 12px #58d4a088; }
.status-value.offline i { background:#e47d83;box-shadow:0 0 12px #e47d8388; }
.state-value { margin:14px 0 3px;color:#aaa2b3;font-size:53px;font-weight:780;letter-spacing:-.06em;line-height:1; }
.state-value.ON { color:#5bd6a2; }.state-value.OFF { color:#e2a36d; }
.status-help { margin:7px 0 15px;color:#858090;font-size:11px; }
code { color:#9a92a4;font-size:10px;overflow-wrap:anywhere; }
.commands-card { margin-top:15px;padding:22px; }
.commands-heading { display:flex;align-items:flex-end;justify-content:space-between;gap:20px;margin-bottom:18px; }
.commands-heading h2 { font-size:17px; }
.commands-heading > div > p:last-child { margin:6px 0 0;color:#898393;font-size:11px; }
.disabled-hint { color:#a99f8d;font-size:11px; }
.command-buttons { display:flex;flex-wrap:wrap;gap:10px; }
button { min-height:42px;padding:0 18px;border-radius:10px;font-size:12px;font-weight:700;cursor:pointer; }
button:disabled { opacity:.42;cursor:not-allowed; }
.quiet-button,.reload-button { border:1px solid #4a3b5c;background:#1e1928;color:#d4c1ef; }
.reload-button span { margin-right:7px;font-size:17px;vertical-align:-1px; }
.start-button { border:0;background:linear-gradient(100deg,#8653d9,#b177ff);color:#110b1b; }
.restart-button { border:1px solid #784e48;background:#2a1b20;color:#f0b1a4; }
.command-result { display:grid;grid-template-columns:auto auto 1fr auto;align-items:center;gap:9px;margin-top:19px;padding:13px 14px;border:1px solid #302a39;border-radius:10px;background:#100e15;color:#948d9c;font-size:10px; }
.command-result strong { color:#ded6e8;font-size:11px; }
.command-result code { justify-self:center; }
.command-result time { color:#817a8c;font-size:10px; }
.feedback,.loading-card { margin:14px 0 0;padding:13px 15px;color:#a9a2b0;font-size:12px; }
.feedback.error { border-color:#744144;background:#2b171b;color:#ffb2b3; }
.feedback.success { border-color:#285b4a;background:#10231e;color:#8fe0bd; }
@keyframes pulse { 50% { opacity:.4; } }
@media(max-width:720px) { .page-heading,.commands-heading { align-items:flex-start;flex-direction:column; }.status-grid { grid-template-columns:1fr; }.status-card { min-height:unset; }.command-result { grid-template-columns:auto 1fr; }.command-result code { grid-column:1/-1;justify-self:start; }.command-result time { grid-column:1/-1; } }
</style>
