<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import { startAuthentication } from '@simplewebauthn/browser';
import AdminApp from './AdminApp.vue';

type AuthState = 'loading' | 'setup' | 'login' | 'authenticated' | 'unavailable';

const authState = ref<AuthState>('loading');
const setupToken = ref('');
const email = ref('');
const password = ref('');
const passwordConfirmation = ref('');
const authenticatorCode = ref('');
const remember = ref(true);
const passwordVisible = ref(false);
const busy = ref(false);
const message = ref('');
const authenticatedEmail = ref('');
const authenticatorRequired = ref(false);
const passkeyAvailable = ref(false);
const passkeyBusy = ref(false);
const isFirstSetup = computed(() => authState.value === 'setup');

async function readResponse(response: Response) {
  if (!response.headers.get('content-type')?.toLowerCase().includes('application/json')) {
    throw new Error('The authentication API is unavailable or is not routed to this site.');
  }
  const payload = await response.json().catch(() => ({}));
  if (!response.ok) throw new Error(payload.error || 'The request could not be completed.');
  return payload;
}

async function loadAuthState() {
  try {
    const sessionResponse = await fetch('/api/auth/session', { credentials: 'same-origin' });
    const session = await readResponse(sessionResponse);
    if (session.authenticated) {
      authenticatedEmail.value = session.email;
      authState.value = 'authenticated';
      return;
    }

    const statusResponse = await fetch('/api/auth/status', { credentials: 'same-origin' });
    const status = await readResponse(statusResponse);
    authState.value = status.setupRequired ? 'setup' : 'login';
    authenticatorRequired.value = Boolean(status.totpEnabled);
    passkeyAvailable.value = Boolean(status.passkeyEnabled);
  } catch {
    authState.value = 'unavailable';
  }
}

async function submitCredentials() {
  message.value = '';
  if (isFirstSetup.value && password.value !== passwordConfirmation.value) {
    message.value = 'The passwords do not match.';
    return;
  }

  busy.value = true;
  try {
    const endpoint = isFirstSetup.value ? '/api/auth/setup' : '/api/auth/login';
    const response = await fetch(endpoint, {
      method: 'POST',
      credentials: 'same-origin',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        email: email.value,
        password: password.value,
        totp: authenticatorCode.value,
        remember: remember.value,
        setupToken: isFirstSetup.value ? setupToken.value : undefined,
      }),
    });
    const payload = await readResponse(response);
    authenticatedEmail.value = payload.email;
    authState.value = 'authenticated';
    setupToken.value = '';
    password.value = '';
    passwordConfirmation.value = '';
    authenticatorCode.value = '';
  } catch (error) {
    const detail = error instanceof Error ? error.message : 'The request could not be completed.';
    message.value = detail;
    if (detail.toLowerCase().includes('set up the first administrator')) authState.value = 'setup';
  } finally {
    busy.value = false;
  }
}

async function signInWithPasskey() {
  if (passkeyBusy.value) return;
  message.value = '';
  if (!email.value.trim()) {
    message.value = 'Enter your username first.';
    return;
  }
  passkeyBusy.value = true;
  try {
    const optionsResponse = await fetch('/api/auth/passkey/login/options', {
      method: 'POST', credentials: 'same-origin', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ email: email.value }),
    });
    const options = await readResponse(optionsResponse);
    const credential = await startAuthentication({ optionsJSON: options });
    const verifyResponse = await fetch('/api/auth/passkey/login/verify', {
      method: 'POST', credentials: 'same-origin', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ email: email.value, credential }),
    });
    const payload = await readResponse(verifyResponse);
    authenticatedEmail.value = payload.email;
    authState.value = 'authenticated';
    password.value = '';
  } catch (cause) {
    message.value = cause instanceof Error ? cause.message : 'Passkey sign-in could not be completed.';
  } finally {
    passkeyBusy.value = false;
  }
}

async function signOut() {
  busy.value = true;
  try {
    await fetch('/api/auth/logout', { method: 'POST', credentials: 'same-origin' });
  } finally {
    authenticatedEmail.value = '';
    password.value = '';
    authState.value = 'login';
    busy.value = false;
  }
}

function showPasswordResetHelp() {
  message.value = 'Password reset is not configured. Contact the server administrator.';
}

onMounted(loadAuthState);
</script>

<template>
  <AdminApp v-if="authState === 'authenticated'" :email="authenticatedEmail" @sign-out="signOut" />
  <main v-else class="login-page">
    <section class="login-card" aria-labelledby="login-title">
      <div class="card-heading">
        <div class="brand-row">
          <img class="brand-logo" src="/MyLogo.svg" alt="AutoBootWeb logo" />
          <div class="card-kicker">
            <span class="mini-lock" aria-hidden="true">
              <svg viewBox="0 0 16 16"><rect x="3" y="7" width="10" height="7" rx="1.5"/><path d="M5.5 7V4.8a2.5 2.5 0 0 1 5 0V7"/></svg>
            </span>
            {{ isFirstSetup ? 'FIRST-TIME SETUP' : 'RESTRICTED AREA' }}
          </div>
        </div>
        <h1 id="login-title">{{ authState === 'setup' ? 'Create your admin account' : authState === 'authenticated' ? 'You’re signed in' : authState === 'loading' ? 'Checking access' : authState === 'unavailable' ? 'Service unavailable' : 'Welcome back' }}</h1>
        <p class="card-subtitle">
          {{ authState === 'setup' ? 'Create the first account to secure your AutoBootWeb console.' : authState === 'authenticated' ? `Signed in as ${authenticatedEmail}` : authState === 'loading' ? 'Connecting to the authentication service…' : authState === 'unavailable' ? 'The authentication service could not be reached.' : 'Sign in with your credentials to continue.' }}
        </p>
      </div>

      <div v-if="authState === 'unavailable'" class="unavailable-state" role="alert">
        <p>The authentication service could not be reached. Start the AutoBootWeb server and route <code>/api/auth</code> requests to it.</p>
        <button class="secondary-button" type="button" @click="loadAuthState">Retry connection</button>
      </div>
      <div v-else-if="authState === 'loading'" class="loading-state" role="status">Loading secure sign-in…</div>
      <form v-else-if="authState === 'setup' || authState === 'login'" class="login-form" @submit.prevent="submitCredentials">
        <template v-if="isFirstSetup">
          <label for="setup-token">One-time setup code</label>
          <div class="input-wrap">
            <svg class="input-icon" viewBox="0 0 20 20" aria-hidden="true"><circle cx="7" cy="10" r="4"/><path d="M11 10h6m-2 0v2m-2-2v2"/></svg>
            <input id="setup-token" v-model="setupToken" autocomplete="off" autocapitalize="none" spellcheck="false" type="password" placeholder="Enter the code from the server" required />
          </div>
        </template>
        <label for="email">Username</label>
        <div class="input-wrap">
          <svg class="input-icon" viewBox="0 0 20 20" aria-hidden="true"><rect x="2.5" y="4" width="15" height="12" rx="2"/><path d="m3.5 5 6.5 5 6.5-5"/></svg>
          <input id="email" v-model="email" autocomplete="username" autocapitalize="none" spellcheck="false" type="text" placeholder="Username" maxlength="254" required />
        </div>

        <div v-if="!isFirstSetup && authenticatorRequired" class="totp-login-field">
          <label for="authenticator-code">Authenticator code</label>
          <div class="input-wrap">
            <svg class="input-icon" viewBox="0 0 20 20" aria-hidden="true"><circle cx="10" cy="10" r="7"/><path d="M10 6v4l2.5 1.5"/></svg>
            <input id="authenticator-code" v-model="authenticatorCode" autocomplete="one-time-code" inputmode="numeric" pattern="[0-9]{6}" maxlength="6" placeholder="Enter the six-digit code" required />
          </div>
        </div>

        <div class="password-label-row">
          <label for="password">{{ isFirstSetup ? 'Create password' : 'Password' }}</label>
          <a v-if="!isFirstSetup" href="#forgot-password" class="text-link" @click.prevent="showPasswordResetHelp">Forgot password?</a>
        </div>
        <div class="input-wrap">
          <svg class="input-icon" viewBox="0 0 20 20" aria-hidden="true"><rect x="3" y="8" width="14" height="10" rx="2"/><path d="M6 8V5.8a4 4 0 0 1 8 0V8m-4 4v2"/></svg>
          <input id="password" v-model="password" :autocomplete="isFirstSetup ? 'new-password' : 'current-password'" :minlength="isFirstSetup ? 12 : undefined" maxlength="1024" :type="passwordVisible ? 'text' : 'password'" :placeholder="isFirstSetup ? 'At least 12 characters' : 'Enter your password'" required />
          <button class="reveal-button" type="button" :aria-label="passwordVisible ? 'Hide password' : 'Show password'" @click="passwordVisible = !passwordVisible">
            <svg v-if="!passwordVisible" viewBox="0 0 20 20" aria-hidden="true"><path d="M2 10s2.8-5 8-5 8 5 8 5-2.8 5-8 5-8-5-8-5Z"/><circle cx="10" cy="10" r="2.2"/></svg>
            <svg v-else viewBox="0 0 20 20" aria-hidden="true"><path d="M3 3 17 17M8.6 5.2A8.8 8.8 0 0 1 10 5c5.2 0 8 5 8 5a12 12 0 0 1-2.2 2.8M5.4 6.1C3.2 7.4 2 10 2 10s2.8 5 8 5c.7 0 1.4-.1 2-.3"/><path d="M8.6 8.6a2 2 0 0 0 2.8 2.8"/></svg>
          </button>
        </div>

        <template v-if="isFirstSetup">
          <label for="password-confirmation">Confirm password</label>
          <div class="input-wrap">
            <svg class="input-icon" viewBox="0 0 20 20" aria-hidden="true"><rect x="3" y="8" width="14" height="10" rx="2"/><path d="M6 8V5.8a4 4 0 0 1 8 0V8m-4 4v2"/></svg>
            <input id="password-confirmation" v-model="passwordConfirmation" autocomplete="new-password" :minlength="12" maxlength="1024" :type="passwordVisible ? 'text' : 'password'" placeholder="Enter your password again" required />
          </div>
          <p class="setup-note">Use at least 12 characters. The setup code is stored on the server and can only be used once.</p>
        </template>

        <div v-if="!isFirstSetup" class="form-options">
          <label class="remember-option">
            <input v-model="remember" type="checkbox" />
            <span class="custom-check" aria-hidden="true"><svg viewBox="0 0 12 12"><path d="m2 6 2.4 2.4L10 3"/></svg></span>
            <span>Keep me signed in</span>
          </label>
          <span class="session-note"><span class="session-dot"></span> Secure session</span>
        </div>

        <button class="submit-button" type="submit" :disabled="busy">
          <span>{{ busy ? 'Please wait…' : isFirstSetup ? 'Create admin account' : 'Sign in to your account' }}</span>
          <svg viewBox="0 0 20 20" aria-hidden="true"><path d="M4 10h12m-5-5 5 5-5 5"/></svg>
        </button>
        <button v-if="!isFirstSetup && passkeyAvailable" class="passkey-login-button" type="button" :disabled="busy || passkeyBusy" @click="signInWithPasskey">
          <span aria-hidden="true">⌘</span>{{ passkeyBusy ? 'Waiting for passkey…' : 'Sign in with a passkey' }}
        </button>
        <p v-if="message" class="form-message" role="alert">{{ message }}</p>
        <p v-if="!isFirstSetup" class="auth-note">
          <svg viewBox="0 0 16 16" aria-hidden="true"><path d="M8 1.8 13 4v3.7c0 3.2-2.2 5.4-5 6.8-2.8-1.4-5-3.6-5-6.8V4l5-2.2Z"/><path d="m5.8 7.8 1.5 1.5 3-3.2"/></svg>
          Authentication protected by TLS
        </p>
      </form>

    </section>
  </main>
</template>

<style scoped>
  .login-page {
    --text: #e8edef;
    --muted: #77858d;
    --accent: #b39aff;
    display: grid;
    min-height: 100svh;
    place-items: center;
    padding: 28px 24px;
    background: radial-gradient(ellipse at 50% 0%, rgba(113, 72, 190, .11), transparent 45%), #0b0911;
  }

  .login-card {
    width: min(676px, 100%);
    padding: 56px;
    border: 1px solid #2c2637;
    border-radius: 25px;
    background: linear-gradient(145deg, #15121d, #121019 78%, #100e16);
    box-shadow: 0 28px 90px rgba(0, 0, 0, .19), inset 0 1px rgba(255, 255, 255, .018);
    color: var(--text);
  }

  .card-heading { margin-bottom: 40px; }
  .brand-row { display: flex; align-items: center; gap: 11px; margin-bottom: 27px; }
  .brand-logo { width: 28px; height: 28px; border-radius: 6px; object-fit: contain; }
  .card-kicker { display: flex; align-items: center; gap: 9px; color: #b49bfa; font-size: 11px; font-weight: 750; letter-spacing: .16em; }
  .mini-lock { display: grid; width: 27px; height: 27px; place-items: center; border: 1px solid rgba(179, 154, 255, .27); border-radius: 7px; background: rgba(179, 154, 255, .06); }
  .mini-lock svg { width: 15px; height: 15px; fill: none; stroke: currentColor; stroke-width: 1.2; stroke-linecap: round; stroke-linejoin: round; }
  h1 { margin: 0; font-size: 37px; font-weight: 640; letter-spacing: -.045em; line-height: 1.2; }
  .card-subtitle { margin: 14px 0 0; color: var(--muted); font-size: 16px; }

  .login-form { display: flex; flex-direction: column; }
  .login-form > label, .password-label-row label { margin: 0 0 13px; color: #c2cbcd; font-size: 14px; font-weight: 620; }
  .input-wrap { position: relative; display: flex; height: 68px; align-items: center; margin-bottom: 29px; border: 1px solid #342d40; border-radius: 12px; background: #100d16; transition: border-color .18s, box-shadow .18s; }
  .input-wrap:focus-within { border-color: rgba(179, 154, 255, .72); box-shadow: 0 0 0 3px rgba(179, 154, 255, .1); }
  .input-icon { flex: 0 0 auto; width: 21px; height: 21px; margin-left: 20px; fill: none; stroke: #78868d; stroke-width: 1.45; stroke-linecap: round; stroke-linejoin: round; }
  .input-wrap input { width: 100%; height: 100%; padding: 0 19px; border: 0; outline: none; background: transparent; color: var(--text); font-size: 16px; }
  .input-wrap input::placeholder { color: #627079; opacity: 1; }
  .password-label-row { display: flex; align-items: center; justify-content: space-between; }
  .password-label-row label { margin-bottom: 13px; }
  .text-link { margin-bottom: 13px; color: #b39aff; font-size: 13px; text-decoration: none; }
  .text-link:hover, .support-copy a:hover { color: #d1c1ff; text-decoration: underline; }
  .reveal-button { display: grid; width: 55px; height: 100%; flex: 0 0 auto; place-items: center; border: 0; background: none; color: #78868d; cursor: pointer; }
  .reveal-button:hover { color: #c6d2d2; }
  .reveal-button svg { width: 21px; height: 21px; fill: none; stroke: currentColor; stroke-width: 1.4; stroke-linecap: round; stroke-linejoin: round; }

  .form-options { display: flex; align-items: center; justify-content: space-between; margin: -1px 0 30px; }
  .remember-option { display: flex; align-items: center; gap: 12px; color: #a7b2b4; font-size: 13px; cursor: pointer; }
  .remember-option input { position: absolute; width: 1px; height: 1px; opacity: 0; }
  .custom-check { display: grid; width: 21px; height: 21px; place-items: center; border: 1px solid #51465f; border-radius: 6px; background: #100d16; transition: .15s; }
  .custom-check svg { width: 15px; height: 15px; fill: none; stroke: #20163b; stroke-width: 2; stroke-linecap: round; stroke-linejoin: round; opacity: 0; }
  .remember-option input:checked + .custom-check { border-color: var(--accent); background: var(--accent); }
  .remember-option input:checked + .custom-check svg { opacity: 1; }
  .remember-option input:focus-visible + .custom-check { outline: 2px solid var(--accent); outline-offset: 3px; }
  .session-note { display: inline-flex; align-items: center; gap: 8px; color: #77858a; font-size: 12px; }
  .session-dot { width: 7px; height: 7px; border-radius: 50%; background: var(--accent); box-shadow: 0 0 9px rgba(179, 154, 255, .55); }
  .submit-button { display: flex; height: 68px; align-items: center; justify-content: center; gap: 19px; border: 1px solid #a78bfa; border-radius: 12px; background: linear-gradient(110deg, #8054e8, #a27ff5); color: #fff; font-size: 16px; font-weight: 750; cursor: pointer; box-shadow: 0 7px 22px rgba(126, 87, 224, .24); transition: filter .18s, transform .18s, box-shadow .18s; }
  .totp-login-field { margin-top: 18px; }
  .passkey-login-button { display:flex;align-items:center;justify-content:center;gap:10px;width:100%;min-height:52px;margin-top:12px;border:1px solid #40364e;border-radius:10px;background:#17141d;color:#d2c9dc;font-size:14px;font-weight:650;cursor:pointer; }
  .passkey-login-button:hover { border-color:#7959a5;background:#1d1727; }
  .passkey-login-button:disabled { opacity:.6;cursor:wait; }
  .submit-button:hover { filter: brightness(1.08); transform: translateY(-1px); box-shadow: 0 9px 28px rgba(126, 87, 224, .34); }
  .submit-button:active { transform: translateY(0); }
  .submit-button svg { width: 20px; height: 20px; fill: none; stroke: currentColor; stroke-width: 1.7; stroke-linecap: round; stroke-linejoin: round; }
  .form-message { margin: 15px 0 0; padding: 12px 14px; border: 1px solid rgba(231, 180, 95, .25); border-radius: 8px; background: rgba(231, 180, 95, .07); color: #e5c68e; font-size: 12px; line-height: 1.6; }
  .form-message code { color: #d7c8ff; }
  .unavailable-state p { margin: 0 0 20px; color: #e5c68e; font-size: 13px; line-height: 1.65; }
  .setup-note { margin: -11px 0 24px; color: #9290a0; font-size: 12px; line-height: 1.6; }
  .loading-state { padding: 22px 0; color: #a8a0b8; font-size: 14px; text-align: center; }
  .signed-in-state { text-align: center; }
  .signed-in-state p { margin: 18px 0 24px; color: #a8a0b8; font-size: 14px; }
  .signed-in-mark { display: grid; width: 48px; height: 48px; margin: 0 auto; place-items: center; border: 1px solid rgba(179, 154, 255, .35); border-radius: 50%; background: rgba(179, 154, 255, .1); color: #c3adff; font-size: 24px; }
  .auth-note { display: flex; align-items: center; justify-content: center; gap: 9px; margin: 19px 0 0; color: #6d7a80; font-size: 12px; }
  .auth-note svg { width: 17px; height: 17px; fill: none; stroke: #819087; stroke-width: 1.25; stroke-linecap: round; stroke-linejoin: round; }

  .secondary-button { display: flex; width: 100%; height: 62px; align-items: center; justify-content: center; border: 1px solid #382f47; border-radius: 12px; background: #15121c; color: #c9c2d3; font-size: 14px; font-weight: 620; cursor: pointer; transition: .18s; }
  .secondary-button:disabled { cursor: wait; opacity: .7; }
  .secondary-button:hover { border-color: #706088; background: #1b1625; }

  @media (max-width: 600px) {
    .login-page { padding: 20px 16px; }
    .login-card { padding: 36px 25px; border-radius: 20px; }
    .card-heading { margin-bottom: 32px; }
    .brand-row { margin-bottom: 23px; }
    h1 { font-size: 31px; }
    .card-subtitle { font-size: 14px; }
    .input-wrap { height: 58px; margin-bottom: 24px; }
    .submit-button { height: 60px; font-size: 14px; }
    .form-options { margin-bottom: 24px; }
  }

  @media (max-width: 370px) {
    .login-card { padding: 30px 19px; }
    .remember-option { gap: 8px; font-size: 12px; }
    .session-note { gap: 6px; font-size: 10px; }
  }

  @media (prefers-reduced-motion: reduce) {
    *, *::before, *::after { scroll-behavior: auto !important; transition-duration: .01ms !important; }
  }
</style>
