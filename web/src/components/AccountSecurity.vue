<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import { startAuthentication, startRegistration } from '@simplewebauthn/browser';


type Passkey = { id: string; name: string; createdAt: string };
type Account = { email: string; totpEnabled: boolean; passkeys: Passkey[] };
type TotpSetup = { secret: string; qr: string; expiresIn: number };

const account = ref<Account | null>(null);
const notice = ref('');
const error = ref('');
const busy = ref(false);
const password = ref({ current: '', next: '', confirmation: '' });
const passkeyProof = ref({ current: '', name: '' });
const totpProof = ref({ current: '', verification: '' });
const removeProof = ref({ current: '', code: '' });
const removeTarget = ref<Passkey | null>(null);
const factorMethod = ref<'totp' | 'passkey'>('totp');
const factorCode = ref('');
const setup = ref<TotpSetup | null>(null);
const factorOptions = computed(() => [
  ...(account.value?.totpEnabled ? [{ label: 'Authenticator code', value: 'totp' as const }] : []),
  ...(account.value?.passkeys.length ? [{ label: 'Passkey', value: 'passkey' as const }] : []),
]);
const hasSecurityFactor = computed(() => factorOptions.value.length > 0);

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
  const result = await api('/api/auth/account');
  account.value = result.account;
  if (!factorOptions.value.some((item) => item.value === factorMethod.value)) {
    factorMethod.value = factorOptions.value[0]?.value || 'totp';
  }
}

async function currentProof(action: string, currentPassword: string) {
  if (!currentPassword) throw new Error('Enter your current password.');
  if (!hasSecurityFactor.value) return { currentPassword };
  if (factorMethod.value === 'totp') {
    if (!factorCode.value) throw new Error('Enter a current authenticator code.');
    return { currentPassword, factor: { method: 'totp', code: factorCode.value } };
  }
  const options = await api('/api/auth/reauth/options', 'POST', { action });
  const credential = await startAuthentication({ optionsJSON: options });
  return { currentPassword, factor: { method: 'passkey', credential } };
}

async function perform(task: () => Promise<void>, success: string) {
  if (busy.value) return;
  busy.value = true;
  notice.value = '';
  error.value = '';
  try {
    await task();
    notice.value = success;
    factorCode.value = '';
  } catch (cause) {
    error.value = cause instanceof Error ? cause.message : 'The request could not be completed.';
  } finally {
    busy.value = false;
  }
}

async function changePassword() {
  if (password.value.next !== password.value.confirmation) {
    error.value = 'The new passwords do not match.';
    return;
  }
  await perform(async () => {
    const proof = await currentProof('change-password', password.value.current);
    await api('/api/auth/password/change', 'POST', {
      ...proof,
      newPassword: password.value.next,
    });
    password.value = { current: '', next: '', confirmation: '' };
    await refresh();
  }, 'Password changed. Other sessions have been signed out.');
}

async function addPasskey() {
  await perform(async () => {
    const proof = await currentProof('add-passkey', passkeyProof.value.current);
    const options = await api('/api/auth/passkeys/options', 'POST', proof);
    const credential = await startRegistration({ optionsJSON: options });
    await api('/api/auth/passkeys/verify', 'POST', { credential, name: passkeyProof.value.name });
    passkeyProof.value = { current: '', name: '' };
    await refresh();
  }, 'Passkey added to your account.');
}

async function removePasskey(passkey: Passkey) {
  await perform(async () => {
    const proof = await currentProof(`remove-passkey:${passkey.id}`, removeProof.value.current);
    await api(`/api/auth/passkeys/${encodeURIComponent(passkey.id)}`, 'DELETE', proof);
    removeProof.value = { current: '', code: '' };
    removeTarget.value = null;
    await refresh();
  }, 'Passkey removed. Other sessions have been signed out.');
}

async function beginTotp() {
  await perform(async () => {
    const proof = await currentProof('enable-totp', totpProof.value.current);
    const result = await api('/api/auth/totp/setup', 'POST', proof);
    setup.value = result;
    totpProof.value.current = '';
  }, 'Scan the QR code and enter a current code to finish setup.');
}

async function verifyTotp() {
  await perform(async () => {
    await api('/api/auth/totp/verify', 'POST', { code: totpProof.value.verification });
    setup.value = null;
    totpProof.value = { current: '', verification: '' };
    await refresh();
  }, 'Authenticator app enabled.');
}

async function disableTotp() {
  await perform(async () => {
    const proof = await currentProof('disable-totp', totpProof.value.current);
    await api('/api/auth/totp', 'DELETE', proof);
    setup.value = null;
    totpProof.value = { current: '', verification: '' };
    await refresh();
  }, 'Authenticator app disabled. Other sessions have been signed out.');
}

function togglePasskeyRemoval(passkey: Passkey) {
  if (removeTarget.value?.id === passkey.id) {
    removeTarget.value = null;
    removeProof.value = { current: '', code: '' };
    factorCode.value = '';
    return;
  }
  removeTarget.value = passkey;
  removeProof.value = { current: '', code: '' };
  factorCode.value = '';
}

onMounted(() => refresh().catch((cause) => { error.value = cause instanceof Error ? cause.message : 'Could not load account security.'; }));
</script>

<template>
  <section class="security-page" aria-labelledby="security-title">
    <header class="section-heading">
      <div>
        <p class="eyebrow">SETTINGS</p>
        <h2 id="security-title">Account security</h2>
        <p class="muted">Manage your username, password and sign-in methods.</p>
      </div>
      <span v-if="account" class="secure-pill"><i></i> Account protected</span>
    </header>

    <p v-if="error" class="feedback error" role="alert">{{ error }}</p>
    <p v-if="notice" class="feedback success" role="status">{{ notice }}</p>
    <div v-if="!account" class="loading-panel">Loading security settings…</div>
    <template v-else>
      <article class="security-card">
        <div class="card-title"><span class="icon">⌑</span><div><h3>Change password</h3><p>Choose a new password with at least 12 characters.</p></div></div>
        <form class="form-grid" @submit.prevent="changePassword">
          <label>Current password<input v-model="password.current" type="password" autocomplete="current-password" required /></label>
          <div v-if="hasSecurityFactor" class="factor-picker">
            <p>Verify with one registered security method</p>
            <div class="factor-options"><label v-for="factor in factorOptions" :key="factor.value"><input v-model="factorMethod" type="radio" :value="factor.value" />{{ factor.label }}</label></div>
            <label v-if="factorMethod === 'totp'">Authenticator code<input v-model="factorCode" type="text" inputmode="numeric" autocomplete="one-time-code" pattern="[0-9]{6}" maxlength="6" required /></label>
          </div>
          <label>New password<input v-model="password.next" type="password" autocomplete="new-password" minlength="12" maxlength="1024" required /></label>
          <label>Confirm new password<input v-model="password.confirmation" type="password" autocomplete="new-password" minlength="12" maxlength="1024" required /></label>
          <div class="form-actions"><button class="primary-button" :disabled="busy">{{ busy ? 'Please wait…' : 'Update password' }}</button></div>
        </form>
      </article>

      <article class="security-card">
        <div class="card-title"><span class="icon">⌘</span><div><h3>Passkeys</h3><p>Sign in securely with a device, security key, or password manager.</p></div></div>
        <ul v-if="account.passkeys.length" class="passkey-list">
          <li v-for="key in account.passkeys" :key="key.id">
            <div class="passkey-item"><span class="passkey-symbol">⌘</span><span class="passkey-name"><strong>{{ key.name }}</strong><small>Added {{ new Date(key.createdAt).toLocaleDateString() }}</small></span><button class="quiet-button danger-button" type="button" :disabled="busy" @click="togglePasskeyRemoval(key)">{{ removeTarget?.id === key.id ? 'Cancel' : 'Remove' }}</button></div>
            <form v-if="removeTarget?.id === key.id" class="remove-form" @submit.prevent="removePasskey(key)">
              <label>Current password<input v-model="removeProof.current" type="password" autocomplete="current-password" required /></label>
              <div v-if="hasSecurityFactor" class="factor-picker">
                <p>Verify with one registered security method</p>
                <div class="factor-options"><label v-for="factor in factorOptions" :key="factor.value"><input v-model="factorMethod" type="radio" :value="factor.value" />{{ factor.label }}</label></div>
                <label v-if="factorMethod === 'totp'">Authenticator code<input v-model="factorCode" type="text" inputmode="numeric" autocomplete="one-time-code" pattern="[0-9]{6}" maxlength="6" required /></label>
              </div>
              <button class="quiet-button danger-button" :disabled="busy">Confirm passkey removal</button>
            </form>
          </li>
        </ul>
        <p v-else class="empty-note">No passkeys have been added yet.</p>
        <form class="form-grid passkey-form" @submit.prevent="addPasskey">
          <label>Passkey name<input v-model="passkeyProof.name" type="text" maxlength="64" placeholder="This device" /></label>
          <label>Current password<input v-model="passkeyProof.current" type="password" autocomplete="current-password" required /></label>
          <div v-if="hasSecurityFactor" class="factor-picker">
            <p>Verify with one registered security method</p>
            <div class="factor-options"><label v-for="factor in factorOptions" :key="factor.value"><input v-model="factorMethod" type="radio" :value="factor.value" />{{ factor.label }}</label></div>
            <label v-if="factorMethod === 'totp'">Authenticator code<input v-model="factorCode" type="text" inputmode="numeric" autocomplete="one-time-code" pattern="[0-9]{6}" maxlength="6" required /></label>
          </div>
          <div class="form-actions"><button class="primary-button" :disabled="busy">{{ busy ? 'Please wait…' : 'Add a passkey' }}</button></div>
          <p class="helper-text">Confirm with your current password and one registered security method. Passkeys require HTTPS or localhost.</p>
        </form>
      </article>

      <article class="security-card">
        <div class="card-title"><span class="icon">◉</span><div><h3>Authenticator app (TOTP)</h3><p>Add a time-based code as a second step for password sign-in.</p></div></div>
        <template v-if="!account.totpEnabled">
          <div v-if="!setup">
            <form class="form-grid compact-form" @submit.prevent="beginTotp">
              <label>Current password<input v-model="totpProof.current" type="password" autocomplete="current-password" required /></label>
              <div v-if="hasSecurityFactor" class="factor-picker">
                <p>Verify with one registered security method</p>
                <div class="factor-options"><label v-for="factor in factorOptions" :key="factor.value"><input v-model="factorMethod" type="radio" :value="factor.value" />{{ factor.label }}</label></div>
                <label v-if="factorMethod === 'totp'">Authenticator code<input v-model="factorCode" type="text" inputmode="numeric" autocomplete="one-time-code" pattern="[0-9]{6}" maxlength="6" required /></label>
              </div>
              <div class="form-actions"><button class="primary-button" :disabled="busy">Set up authenticator</button></div>
            </form>
          </div>
          <div v-else class="totp-setup">
            <div class="qr-frame"><img :src="setup.qr" alt="QR code for authenticator setup" width="220" height="220" /></div>
            <div class="totp-instructions"><h4>Scan this code</h4><p>Use Google Authenticator, KeePassXC, or another TOTP app. Setup expires in 10 minutes.</p><label>Manual setup key<code>{{ setup.secret }}</code></label>
              <form class="form-grid compact-form" @submit.prevent="verifyTotp"><label>Six-digit code<input v-model="totpProof.verification" type="text" inputmode="numeric" autocomplete="one-time-code" pattern="[0-9]{6}" maxlength="6" required /></label><div class="form-actions"><button class="primary-button" :disabled="busy">Verify and enable</button><button class="quiet-button" type="button" :disabled="busy" @click="setup = null">Cancel</button></div></form>
            </div>
          </div>
        </template>
        <template v-else>
          <div class="enabled-row"><span class="enabled-dot"></span><span><strong>Authenticator enabled</strong><small>Codes are required for password sign-in and sensitive account changes.</small></span></div>
          <form class="form-grid compact-form" @submit.prevent="disableTotp">
            <label>Current password<input v-model="totpProof.current" type="password" autocomplete="current-password" required /></label>
            <div v-if="hasSecurityFactor" class="factor-picker">
              <p>Verify with one registered security method</p>
              <div class="factor-options"><label v-for="factor in factorOptions" :key="factor.value"><input v-model="factorMethod" type="radio" :value="factor.value" />{{ factor.label }}</label></div>
              <label v-if="factorMethod === 'totp'">Authenticator code<input v-model="factorCode" type="text" inputmode="numeric" autocomplete="one-time-code" pattern="[0-9]{6}" maxlength="6" required /></label>
            </div>
            <div class="form-actions"><button class="quiet-button danger-button" :disabled="busy">Disable authenticator</button></div>
          </form>
        </template>
      </article>
    </template>
  </section>
</template>

<style scoped>
.security-page { max-width: 980px; margin: 0 auto; color: #e8e5f0; }
.section-heading { display:flex; align-items:flex-end; justify-content:space-between; gap:20px; margin: 8px 0 26px; }
.eyebrow { margin:0 0 9px; color:#b69aff; font-size:11px; font-weight:750; letter-spacing:.17em; }
h2 { margin:0; font-size:clamp(25px, 4vw, 34px); letter-spacing:-.04em; }
.muted, .card-title p { color:#928d9d; }
.muted { margin:8px 0 0; font-size:14px; }
.secure-pill { display:flex; align-items:center; gap:8px; white-space:nowrap; color:#a8a0b4; font-size:12px; }
.secure-pill i,.enabled-dot { width:8px;height:8px;border-radius:50%;background:#55d6a3;box-shadow:0 0 14px #55d6a377; }
.security-card,.loading-panel { margin:14px 0; padding:24px; border:1px solid #2d2838; border-radius:18px; background:linear-gradient(145deg,#16131f,#121018); }
.card-title { display:flex; align-items:flex-start; gap:13px; margin-bottom:20px; }
.card-title .icon { display:grid;place-items:center;width:36px;height:36px;border:1px solid #51416e;border-radius:11px;color:#c4aaff;font-size:19px; }
h3 { margin:0; font-size:16px; }
.card-title p { margin:5px 0 0; font-size:13px; line-height:1.55; }
.form-grid { display:grid; grid-template-columns:repeat(2,minmax(0,1fr)); gap:15px; }
label { display:flex; flex-direction:column; gap:8px; color:#c6c0d0; font-size:12px; font-weight:620; }
input { width:100%; min-height:43px; padding:0 12px; border:1px solid #373141; border-radius:10px; outline:0; background:#0d0b12; color:#eeeaf5; font-size:14px; }
input:focus { border-color:#9871e8; box-shadow:0 0 0 3px #9871e81c; }
.form-actions { display:flex; align-items:center; gap:10px; align-self:end; }
button { min-height:40px; padding:0 16px; border-radius:10px; color:#eeeaf5; cursor:pointer; font-weight:650; }
button:disabled { opacity:.55; cursor:wait; }
.primary-button { border:0; background:linear-gradient(100deg,#8753db,#b177ff); color:#100b19; }
.quiet-button { border:1px solid #41394c; background:#17141e; }
.danger-button { color:#ffaaa9; }
.helper-text,.empty-note { margin:0; color:#827d8b; font-size:12px; line-height:1.5; }
.passkey-form { border-top:1px solid #292430; padding-top:17px; }
.passkey-list { list-style:none; padding:0; margin:0 0 16px; }
.passkey-list li { display:block;padding:12px 0;border-bottom:1px solid #282430; }
.passkey-item { display:flex;align-items:center;gap:12px; }
.passkey-symbol { color:#b79aff;font-size:20px; }
.passkey-name { display:flex;flex:1;flex-direction:column;gap:4px; }
.passkey-name strong { font-size:13px; }
.passkey-name small,.enabled-row small { color:#817a8a;font-size:11px; }
.remove-form { display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:13px;margin:14px 0 3px 31px;padding:15px;border:1px solid #453044;border-radius:12px;background:#1c141b; }
.factor-picker { display:grid;gap:9px;align-content:start; }
.factor-picker > p { margin:0;color:#a49bad;font-size:11px; }
.factor-options { display:flex;flex-wrap:wrap;gap:8px; }
.factor-options label { display:flex;flex-direction:row;align-items:center;gap:7px;padding:8px 10px;border:1px solid #393343;border-radius:9px;background:#100e15;color:#c3bdcc;font-size:11px;cursor:pointer; }
.factor-options input { width:14px;min-width:14px;min-height:14px;height:14px;margin:0;padding:0;accent-color:#a77bec; }
.factor-picker > label { margin-top:2px; }
.compact-form { max-width:470px; }
.totp-setup { display:grid;grid-template-columns:240px 1fr;gap:24px;align-items:start; }
.qr-frame { display:grid;place-items:center;padding:10px;border-radius:13px;background:white; }
.qr-frame img { display:block;max-width:100%;height:auto; }
.totp-instructions h4 { margin:0 0 7px; }
.totp-instructions p { margin:0 0 17px;color:#938d9d;font-size:13px;line-height:1.55; }
.totp-instructions label { margin-bottom:15px; }
code { display:block; overflow-wrap:anywhere;padding:12px;border:1px solid #3c3449;border-radius:9px;background:#0d0b12;color:#d6c2ff;font-size:13px;letter-spacing:.08em; }
.enabled-row { display:flex;align-items:center;gap:10px;margin:8px 0 18px;font-size:13px; }
.enabled-row span:last-child { display:flex;flex-direction:column;gap:5px; }
.feedback { margin:14px 0;padding:12px 15px;border-radius:10px;font-size:13px; }
.feedback.error { border:1px solid #744144;background:#2b171b;color:#ffb2b3; }
.feedback.success { border:1px solid #285b4a;background:#10231e;color:#8fe0bd; }
.loading-panel { color:#8e8897;font-size:14px; }
@media(max-width:700px) { .security-card { padding:18px; } .form-grid,.totp-setup,.remove-form { grid-template-columns:1fr; } .totp-setup { gap:17px; } .qr-frame { width:240px;max-width:100%; } .section-heading { align-items:flex-start;flex-direction:column; } .remove-form { margin-left:0; } }
</style>
