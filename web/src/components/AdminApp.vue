<script setup lang="ts">
import { onMounted, onUnmounted, ref } from 'vue';
import AccountSecurity from './AccountSecurity.vue';
import BrokerConfiguration from './BrokerConfiguration.vue';
import DeviceControl from './DeviceControl.vue';
import UsbSetup from './UsbSetup.vue';

defineProps<{ email: string }>();
defineEmits<{ (event: 'sign-out'): void }>();

const page = ref<'control' | 'settings'>('control');
const controlTab = ref<'control' | 'usb'>('control');
const settingsTab = ref<'broker' | 'security'>('broker');
const brokerConnection = ref<'checking' | 'unknown' | 'connected' | 'connecting' | 'subscribing' | 'subscription-issue' | 'disconnected'>('checking');
let brokerStatusTimer: ReturnType<typeof setInterval> | undefined;

async function refreshBrokerStatus() {
  try {
    const response = await fetch('/api/auth/broker/status', { credentials: 'same-origin' });
    if (!response.ok) throw new Error('Broker status is unavailable.');
    const result = await response.json();
    brokerConnection.value = result.connection.connected
      ? result.connection.subscriptionsReady
        ? 'connected'
        : result.connection.lastError ? 'subscription-issue' : 'subscribing'
      : result.connection.connecting ? 'connecting' : 'disconnected';
  } catch {
    brokerConnection.value = 'unknown';
  }
}

onMounted(() => {
  refreshBrokerStatus();
  brokerStatusTimer = setInterval(refreshBrokerStatus, 3000);
});
onUnmounted(() => { if (brokerStatusTimer) clearInterval(brokerStatusTimer); });
</script>

<template>
  <div class="admin-shell">
    <aside class="sidebar">
      <a class="brand" href="#control" @click.prevent="page = 'control'"><img src="/MyLogo.svg" alt="AutoBootWeb" /><span>AutoBootWeb</span></a>
      <div class="nav-label">WORKSPACE</div>
      <button class="nav-item" :class="{ active: page === 'control' }" @click="page = 'control'"><span>⌘</span> Control</button>
      <button class="nav-item" :class="{ active: page === 'settings' }" @click="page = 'settings'"><span>⚙</span> Settings</button>
      <div class="sidebar-spacer"></div>
      <div class="account-block"><span class="avatar">{{ email.includes('@') ? 'A' : email.slice(0, 1).toUpperCase() }}</span><span class="account-email">{{ email.includes('@') ? 'Account' : email }}</span><button class="signout" title="Sign out" aria-label="Sign out" @click="$emit('sign-out')">↗</button></div>
    </aside>

    <main class="main-area">
      <header class="topbar">
        <span>{{ page === 'control' ? 'Device control' : 'Settings' }}</span>
        <div class="header-status-group">
          <span class="broker-indicator" :class="brokerConnection" role="status" aria-live="polite"><i></i>{{ brokerConnection === 'subscription-issue' ? 'Broker topic access issue' : brokerConnection === 'subscribing' ? 'Broker subscribing' : `Broker ${brokerConnection}` }}</span>
        </div>
      </header>
      <section v-if="page === 'control'" class="control-view">
        <div class="tabs" role="tablist" aria-label="Control sections">
          <button role="tab" :aria-selected="controlTab === 'control'" :class="{ selected: controlTab === 'control' }" @click="controlTab = 'control'">Device control</button>
          <button role="tab" :aria-selected="controlTab === 'usb'" :class="{ selected: controlTab === 'usb' }" @click="controlTab = 'usb'">USB setup</button>
        </div>
        <DeviceControl v-if="controlTab === 'control'" role="tabpanel" @open-settings="page = 'settings'; settingsTab = 'broker'" />
        <UsbSetup v-else role="tabpanel" />
      </section>

      <section v-else class="settings-view">
        <header class="settings-heading"><p class="eyebrow">PREFERENCES</p><h1>Settings</h1><p>Configure broker access and manage account security.</p></header>
        <div class="tabs" role="tablist" aria-label="Settings sections">
          <button role="tab" :aria-selected="settingsTab === 'broker'" :class="{ selected: settingsTab === 'broker' }" @click="settingsTab = 'broker'">Broker connection</button>
          <button role="tab" :aria-selected="settingsTab === 'security'" :class="{ selected: settingsTab === 'security' }" @click="settingsTab = 'security'">Account security</button>
        </div>
        <BrokerConfiguration v-if="settingsTab === 'broker'" role="tabpanel" />
        <AccountSecurity v-else role="tabpanel" />
      </section>
    </main>
  </div>
</template>

<style scoped>
.admin-shell { --accent:#b89aff; --accent-deep:#9464df; display:grid;grid-template-columns:250px minmax(0,1fr);min-height:100svh;color:#eae7f1;background:radial-gradient(ellipse at 62% 0%,#40215b25,transparent 42%),#0c0a11; }
.sidebar { display:flex;flex-direction:column;min-height:100svh;padding:25px 16px 17px;border-right:1px solid #26222e;background:#100e16; }
.brand { display:flex;align-items:center;gap:11px;margin:0 5px 43px;color:#f0edf6;text-decoration:none;font-size:14px;font-weight:750;letter-spacing:-.02em; }
.brand img { width:29px;height:29px;object-fit:contain; }
.nav-label { margin:0 11px 10px;color:#696373;font-size:10px;font-weight:750;letter-spacing:.16em; }
.nav-item { display:flex;align-items:center;gap:12px;width:100%;height:43px;margin:3px 0;padding:0 11px;border:1px solid transparent;border-radius:10px;background:transparent;color:#96909f;text-align:left;font-size:13px;cursor:pointer; }
.nav-item span { display:grid;place-items:center;width:19px;color:#837b91;font-size:19px; }
.nav-item:hover { background:#191620;color:#ded8e8; }
.nav-item.active { border-color:#47345f;background:#21182e;color:#d4bcff; }
.nav-item.active span { color:#c39fff; }
.sidebar-spacer { flex:1; }
.account-block { display:flex;align-items:center;gap:10px;padding:13px 5px 0;border-top:1px solid #28242f; }
.avatar { display:grid;place-items:center;flex:0 0 31px;height:31px;border:1px solid #614991;border-radius:50%;background:#271c39;color:#d5bfff;font-weight:750;font-size:13px; }
.account-email { overflow:hidden;flex:1;color:#aba5b3;font-size:11px;text-overflow:ellipsis;white-space:nowrap; }
.signout { border:0;background:transparent;color:#827b8b;font-size:18px;cursor:pointer; }
.main-area { min-width:0; }
.topbar { display:flex;align-items:center;justify-content:space-between;height:67px;padding:0 36px;border-bottom:1px solid #211e28;color:#a6a0ae;font-size:12px; }
.header-status-group { display:flex;align-items:center;gap:18px; }
.secure { display:flex;align-items:center;gap:8px;color:#777281; }
.secure i { width:7px;height:7px;border-radius:50%;background:#57d5a0;box-shadow:0 0 13px #57d5a080; }
.broker-indicator { display:flex;align-items:center;gap:8px;color:#817a8b;text-transform:capitalize; }
.broker-indicator i { width:7px;height:7px;border-radius:50%;background:#77717f; }
.broker-indicator.connected { color:#91d9b5; }
.broker-indicator.connected i { background:#58d4a0;box-shadow:0 0 12px #58d4a099; }
.broker-indicator.connecting { color:#e3c27f; }
.broker-indicator.connecting i { background:#e5bd6b;box-shadow:0 0 12px #e5bd6b88;animation:broker-pulse 1.2s infinite; }
.broker-indicator.subscribing { color:#e3c27f; }
.broker-indicator.subscribing i { background:#e5bd6b;box-shadow:0 0 12px #e5bd6b88;animation:broker-pulse 1.2s infinite; }
.broker-indicator.subscription-issue { color:#f0a0a4; }
.broker-indicator.subscription-issue i { background:#df797f; }
.broker-indicator.disconnected { color:#bd8588; }
.broker-indicator.disconnected i { background:#df797f; }
@keyframes broker-pulse { 50% { opacity:.4; } }
.control-view { padding:30px 36px 60px; }
.eyebrow { margin:0 0 10px;color:#bb9cff;font-size:10px;font-weight:780;letter-spacing:.19em; }
h1 { margin:0;color:#efedf3;font-size:clamp(28px,4vw,40px);letter-spacing:-.045em; }
.settings-view { max-width:1120px;margin:0 auto;padding:30px 36px 60px; }
.settings-heading { margin:0 0 23px; }
.settings-heading h1 { font-size:31px; }
.settings-heading > p:last-child { margin:8px 0 0;color:#8e8898;font-size:13px; }
.tabs { display:flex;gap:7px;margin-bottom:24px;padding:5px;border:1px solid #282430;border-radius:12px;background:#121018;width:max-content;max-width:100%; }
.tabs button { min-height:35px;padding:0 15px;border:1px solid transparent;border-radius:8px;background:transparent;color:#918a9b;font-size:12px;font-weight:650;cursor:pointer; }
.tabs button.selected { border-color:#483660;background:#241a31;color:#d4bbff; }
@media(max-width:760px) { .admin-shell { grid-template-columns:1fr; } .sidebar { position:sticky;z-index:2;top:0;display:grid;grid-template-columns:1fr auto auto;align-items:center;gap:7px;min-height:unset;padding:10px 13px;border-right:0;border-bottom:1px solid #26222e; } .brand { margin:0 8px 0 0; } .brand span { display:none; } .nav-label,.sidebar-spacer,.account-block { display:none; } .nav-item { width:auto;height:38px;margin:0;padding:0 11px; } .nav-item span { display:none; } .topbar { height:52px;padding:0 17px;font-size:10px; } .header-status-group { gap:10px; }.secure,.broker-indicator { gap:5px;font-size:9px; }.settings-view,.control-view { padding:23px 16px 40px; } }
</style>
