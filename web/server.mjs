import {
  createCipheriv,
  createDecipheriv,
  createPrivateKey,
  createPublicKey,
  randomBytes,
  randomUUID,
  scrypt as scryptCallback,
  timingSafeEqual,
  X509Certificate,
} from 'node:crypto';
import { promisify } from 'node:util';
import { createServer } from 'node:http';
import { createReadStream } from 'node:fs';
import { chmod, link, mkdir, open, readFile, rename, stat, unlink } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  generateAuthenticationOptions,
  generateRegistrationOptions,
  verifyAuthenticationResponse,
  verifyRegistrationResponse,
} from '@simplewebauthn/server';
import * as OTPAuth from 'otpauth';
import QRCode from 'qrcode';
import forge from 'node-forge';
import mqtt from 'mqtt';

const scrypt = promisify(scryptCallback);
const root = path.dirname(fileURLToPath(import.meta.url));
const publicRoot = path.join(root, 'dist');
const dataDirectory = path.resolve(process.env.AUTH_DATA_DIR || path.join(root, '.data'));
const accountFile = path.join(dataDirectory, 'admin.json');
const setupTokenFile = path.join(dataDirectory, 'setup-token');
const totpKeyFile = path.join(dataDirectory, 'totp-encryption.key');
const brokerConfigFile = path.join(dataDirectory, 'broker-config.json');
const brokerKeyPairFile = path.join(dataDirectory, 'broker-keypair.json');
const brokerCertificateFile = path.join(dataDirectory, 'broker-client-cert.pem');
const brokerCaFile = path.join(dataDirectory, 'broker-ca.pem');
const port = Number(process.env.PORT || 18082);
const apiOnly = process.env.API_ONLY === 'true';
const sessionCookie = 'autoboot_session';
const sessionDuration = 12 * 60 * 60 * 1000;
const rememberedSessionDuration = 30 * 24 * 60 * 60 * 1000;
const rateLimitWindow = 5 * 60 * 1000;
const rateLimitAttempts = 10;
const scryptOptions = { N: 32768, r: 8, p: 1, maxmem: 64 * 1024 * 1024 };
const mimeTypes = {
  '.css': 'text/css; charset=utf-8',
  '.html': 'text/html; charset=utf-8',
  '.ico': 'image/x-icon',
  '.js': 'text/javascript; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.png': 'image/png',
  '.svg': 'image/svg+xml',
  '.txt': 'text/plain; charset=utf-8',
  '.webp': 'image/webp',
  '.woff2': 'font/woff2',
};

let administrator = null;
let setupInProgress = false;
const sessions = new Map();
const passkeyChallenges = new Map();
const pendingTotp = new Map();
const rateLimits = new Map();
let passwordChecksInProgress = 0;
const maxConcurrentPasswordChecks = 2;
let brokerClient = null;
const brokerDeviceState = { availability: 'unknown', state: 'unknown', updatedAt: null, lastCommand: null };
const pendingBrokerCommands = new Map();
const brokerConnectionState = {
  connected: false,
  connecting: false,
  handshakeSucceeded: false,
  protocolVersion: null,
  subscriptionsReady: false,
  lastHandshakeAt: null,
  lastError: '',
};

function sendJson(response, statusCode, payload, headers = {}) {
  response.writeHead(statusCode, {
    'Cache-Control': 'no-store',
    'Content-Type': 'application/json; charset=utf-8',
    'X-Content-Type-Options': 'nosniff',
    ...headers,
  });
  response.end(JSON.stringify(payload));
}

function readCookie(request, name) {
  const cookies = request.headers.cookie?.split(';') || [];
  for (const item of cookies) {
    const separator = item.indexOf('=');
    if (separator < 0) continue;
    if (item.slice(0, separator).trim() === name) return item.slice(separator + 1).trim();
  }
  return '';
}

function cookieHeader(token, maxAge) {
  const secure = process.env.COOKIE_SECURE === 'true' || process.env.NODE_ENV === 'production';
  return `${sessionCookie}=${token}; HttpOnly; SameSite=Strict; Path=/; Max-Age=${Math.floor(maxAge / 1000)}${secure ? '; Secure' : ''}`;
}

function clearCookieHeader() {
  const secure = process.env.COOKIE_SECURE === 'true' || process.env.NODE_ENV === 'production';
  return `${sessionCookie}=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0${secure ? '; Secure' : ''}`;
}

function verifySameOrigin(request) {
  const origin = request.headers.origin;
  const host = request.headers.host;
  if (!origin || !host) return false;

  let parsedOrigin;
  try {
    parsedOrigin = new URL(origin);
  } catch {
    return false;
  }

  const forwardedProtocol = process.env.TRUST_PROXY === 'true'
    ? String(request.headers['x-forwarded-proto'] || '').split(',')[0].trim()
    : '';
  const protocol = forwardedProtocol === 'https' || request.socket.encrypted ? 'https:' : 'http:';
  return parsedOrigin.protocol === protocol && parsedOrigin.host.toLowerCase() === host.toLowerCase();
}

function getClientAddress(request) {
  if (process.env.TRUST_PROXY === 'true') {
    const forwardedAddresses = String(request.headers['x-forwarded-for'] || '')
      .split(',')
      .map((address) => address.trim())
      .filter(Boolean);
    const proxyAddress = forwardedAddresses.at(-1);
    if (proxyAddress && proxyAddress.length <= 64) return proxyAddress;
  }
  return request.socket.remoteAddress || 'unknown';
}

function isRateLimited(request) {
  const now = Date.now();
  const key = getClientAddress(request);
  const current = rateLimits.get(key);
  if (!current || current.resetAt <= now) {
    rateLimits.set(key, { attempts: 1, resetAt: now + rateLimitWindow });
    return false;
  }
  current.attempts += 1;
  return current.attempts > rateLimitAttempts;
}

function cleanExpiredState() {
  const now = Date.now();
  for (const [token, session] of sessions) {
    if (session.expiresAt <= now) sessions.delete(token);
  }
  for (const [address, limit] of rateLimits) {
    if (limit.resetAt <= now) rateLimits.delete(address);
  }
  for (const [key, value] of passkeyChallenges) {
    if (value.expiresAt <= now) passkeyChallenges.delete(key);
  }
  for (const [key, value] of pendingTotp) {
    if (value.expiresAt <= now) pendingTotp.delete(key);
  }
}

function currentSession(request) {
  cleanExpiredState();
  const token = readCookie(request, sessionCookie);
  const session = sessions.get(token);
  if (!session || session.expiresAt <= Date.now()) return null;
  return { ...session, token };
}

function createSession(email, remember) {
  const duration = remember ? rememberedSessionDuration : sessionDuration;
  const token = randomBytes(32).toString('base64url');
  sessions.set(token, { email, expiresAt: Date.now() + duration });
  return { token, duration };
}

function normalizeEmail(value) {
  return typeof value === 'string' ? value.trim().toLowerCase() : '';
}

function validEmail(value) {
  return /^[a-z0-9._-]{1,64}$/.test(value);
}

async function readJsonBody(request) {
  if (!String(request.headers['content-type'] || '').toLowerCase().startsWith('application/json')) {
    const error = new Error('Expected a JSON request.');
    error.statusCode = 415;
    throw error;
  }

  let size = 0;
  const chunks = [];
  for await (const chunk of request) {
    size += chunk.length;
    if (size > 128 * 1024) {
      const error = new Error('Request body is too large.');
      error.statusCode = 413;
      throw error;
    }
    chunks.push(chunk);
  }

  try {
    const payload = JSON.parse(Buffer.concat(chunks).toString('utf8'));
    if (!payload || typeof payload !== 'object' || Array.isArray(payload)) throw new Error('Expected an object.');
    return payload;
  } catch {
    const error = new Error('Invalid JSON request.');
    error.statusCode = 400;
    throw error;
  }
}

async function persistAdministrator(email, password) {
  const salt = randomBytes(16);
  const passwordHash = await scrypt(password, salt, 64, scryptOptions);
  const record = {
    email,
    salt: salt.toString('base64url'),
    passwordHash: Buffer.from(passwordHash).toString('base64url'),
    passkeys: [],
    totp: null,
    createdAt: new Date().toISOString(),
  };

  await mkdir(dataDirectory, { recursive: true, mode: 0o700 });
  await chmod(dataDirectory, 0o700);
  const temporaryFile = path.join(dataDirectory, `.admin-${randomBytes(8).toString('hex')}.tmp`);
  const file = await open(temporaryFile, 'wx', 0o600);
  try {
    await file.writeFile(JSON.stringify(record));
    await file.sync();
  } finally {
    await file.close();
  }
  try {
    await link(temporaryFile, accountFile);
  } finally {
    await unlink(temporaryFile).catch(() => {});
  }
  return record;
}

async function saveAdministrator(record) {
  const temporaryFile = path.join(dataDirectory, `.admin-${randomBytes(8).toString('hex')}.tmp`);
  const file = await open(temporaryFile, 'wx', 0o600);
  try {
    await file.writeFile(JSON.stringify(record));
    await file.sync();
  } finally {
    await file.close();
  }
  try {
    await rename(temporaryFile, accountFile);
    await chmod(accountFile, 0o600);
  } catch (error) {
    await unlink(temporaryFile).catch(() => {});
    throw error;
  }
  administrator = record;
}

async function initializeTotpEncryptionKey() {
  try {
    const key = await readFile(totpKeyFile);
    if (key.length !== 32) throw new Error('The TOTP encryption key has an invalid length.');
    await chmod(totpKeyFile, 0o600);
    return key;
  } catch (error) {
    if (error.code !== 'ENOENT') throw error;
  }
  const key = randomBytes(32);
  try {
    const file = await open(totpKeyFile, 'wx', 0o600);
    await file.writeFile(key);
    await file.sync();
    await file.close();
    return key;
  } catch (error) {
    if (error.code !== 'EEXIST') throw error;
    const existing = await readFile(totpKeyFile);
    if (existing.length !== 32) throw new Error('The TOTP encryption key has an invalid length.');
    return existing;
  }
}

let totpEncryptionKey;

function encryptTotpSecret(secret) {
  const iv = randomBytes(12);
  const cipher = createCipheriv('aes-256-gcm', totpEncryptionKey, iv);
  const encrypted = Buffer.concat([cipher.update(secret, 'utf8'), cipher.final()]);
  return Buffer.concat([iv, cipher.getAuthTag(), encrypted]).toString('base64url');
}

function decryptTotpSecret(encrypted) {
  const data = Buffer.from(encrypted, 'base64url');
  if (data.length < 29) throw new Error('The authenticator secret is invalid.');
  const decipher = createDecipheriv('aes-256-gcm', totpEncryptionKey, data.subarray(0, 12));
  decipher.setAuthTag(data.subarray(12, 28));
  return Buffer.concat([decipher.update(data.subarray(28)), decipher.final()]).toString('utf8');
}

function verifyTotp(record, candidate) {
  if (!record || !/^\d{6}$/.test(String(candidate))) return null;
  const totp = new OTPAuth.TOTP({
    issuer: 'AutoBootWeb',
    algorithm: 'SHA1',
    digits: 6,
    period: 30,
    secret: OTPAuth.Secret.fromBase32(decryptTotpSecret(record.encrypted)),
  });
  const delta = totp.validate({ token: String(candidate), window: 1 });
  if (delta === null) return null;
  const step = Math.floor(Date.now() / 30_000) + delta;
  return step > (record.lastStep ?? -1) ? step : null;
}

function rpContext(request) {
  const originHeader = request.headers.origin;
  if (!verifySameOrigin(request)) throw Object.assign(new Error('Request origin could not be verified.'), { statusCode: 403 });
  const origin = new URL(originHeader);
  if (origin.protocol !== 'https:' && !['localhost', '127.0.0.1', '[::1]'].includes(origin.hostname)) {
    throw Object.assign(new Error('Passkeys require HTTPS or localhost.'), { statusCode: 403 });
  }
  return { origin: origin.origin, rpID: origin.hostname };
}

function credentialsFor(account) {
  if (!Array.isArray(account.passkeys)) account.passkeys = [];
  return account.passkeys;
}

function publicAccount(account) {
  return {
    email: account.email,
    hasPassword: true,
    totpEnabled: Boolean(account.totp),
    passkeys: credentialsFor(account).map(({ id, name, createdAt }) => ({ id, name, createdAt })),
  };
}

function authSession(request) {
  const session = currentSession(request);
  if (!session || session.email !== administrator?.email) return null;
  return session;
}

async function verifyCurrentPassword(body, account = administrator) {
  const candidate = typeof body.currentPassword === 'string' && body.currentPassword.length <= 1024
    ? body.currentPassword
    : '';
  const matches = await verifyPassword(candidate);
  return Boolean(matches && account && account.email === administrator?.email);
}

async function verifyCurrentFactor(request, body, action) {
  const hasTotp = Boolean(administrator?.totp);
  const passkeys = administrator ? credentialsFor(administrator) : [];
  if (!hasTotp && !passkeys.length) return true;

  const proof = body.factor;
  if (proof?.method === 'totp' && hasTotp) {
    const step = verifyTotp(administrator.totp, proof.code);
    if (step === null) return false;
    administrator.totp.lastStep = step;
    await saveAdministrator(administrator);
    return true;
  }

  if (proof?.method !== 'passkey' || !passkeys.length) return false;
  const session = authSession(request);
  if (!session) return false;
  const key = `reauth:${session.token}:${action}`;
  const saved = passkeyChallenges.get(key);
  passkeyChallenges.delete(key);
  if (!saved || saved.expiresAt <= Date.now()) return false;
  const credential = passkeys.find((entry) => entry.id === proof.credential?.id);
  if (!credential) return false;
  const context = rpContext(request);
  if (saved.origin !== context.origin || saved.rpID !== context.rpID) return false;
  const verification = await verifyAuthenticationResponse({
    response: proof.credential,
    expectedChallenge: saved.challenge,
    expectedOrigin: saved.origin,
    expectedRPID: saved.rpID,
    requireUserVerification: true,
    credential: {
      id: credential.id,
      publicKey: new Uint8Array(Buffer.from(credential.publicKey, 'base64url')),
      counter: credential.counter || 0,
      transports: credential.transports,
    },
  });
  if (!verification.verified) return false;
  credential.counter = verification.authenticationInfo.newCounter;
  await saveAdministrator(administrator);
  return true;
}

async function verifyCurrentCredentials(request, body, action) {
  return (await verifyCurrentPassword(body)) && verifyCurrentFactor(request, body, action);
}

async function storePrivateFile(filePath, content) {
  await mkdir(dataDirectory, { recursive: true, mode: 0o700 });
  await chmod(dataDirectory, 0o700);
  const temporaryFile = `${filePath}.${randomBytes(8).toString('hex')}.tmp`;
  const file = await open(temporaryFile, 'wx', 0o600);
  try {
    await file.writeFile(content);
    await file.sync();
  } finally {
    await file.close();
  }
  try {
    await rename(temporaryFile, filePath);
    await chmod(filePath, 0o600);
  } catch (error) {
    await unlink(temporaryFile).catch(() => {});
    throw error;
  }
}

async function readOptionalFile(filePath, encoding = 'utf8') {
  try {
    return await readFile(filePath, encoding);
  } catch (error) {
    if (error.code === 'ENOENT') return null;
    throw error;
  }
}

async function readBrokerKeyPair() {
  const contents = await readOptionalFile(brokerKeyPairFile);
  if (!contents) return null;
  const pair = JSON.parse(contents);
  if (typeof pair.privateKey !== 'string' || typeof pair.csr !== 'string') {
    throw new Error('The broker key pair is invalid. Generate a new CSR.');
  }
  return pair;
}

async function readBrokerConfig() {
  const contents = await readOptionalFile(brokerConfigFile);
  if (!contents) return { host: '', port: 8084, path: '/mqtt', deviceId: 'AutoBoot' };
  const config = JSON.parse(contents);
  return { host: config.host || '', port: config.port || 8084, path: config.path || '/mqtt', deviceId: config.deviceId || 'AutoBoot' };
}

async function brokerPublicState() {
  const [config, keyPair, clientCertificate, ca] = await Promise.all([
    readBrokerConfig(),
    readBrokerKeyPair(),
    readOptionalFile(brokerCertificateFile),
    readOptionalFile(brokerCaFile),
  ]);
  return {
    config,
    csr: keyPair?.csr || '',
    clientCertificatePem: clientCertificate || '',
    brokerCaPem: ca || '',
    hasClientCertificate: Boolean(clientCertificate),
    hasBrokerCa: Boolean(ca),
    connection: { ...brokerConnectionState },
  };
}

function normalizeBrokerConfig(body) {
  const hostInput = typeof body.host === 'string' ? body.host.trim() : '';
  const port = Number(body.port);
  const pathInput = typeof body.path === 'string' && body.path.trim() ? body.path.trim() : '/mqtt';
  const deviceId = typeof body.deviceId === 'string' ? body.deviceId.trim() : '';
  if (!hostInput || hostInput.length > 253 || /[\s/@?#]/.test(hostInput) || hostInput.includes('://')) {
    throw Object.assign(new Error('Enter a valid broker host name or IP address.'), { statusCode: 400 });
  }
  if (!Number.isInteger(port) || port < 1 || port > 65535) {
    throw Object.assign(new Error('Enter a valid WebSocket port (1–65535).'), { statusCode: 400 });
  }
  if (!pathInput.startsWith('/') || pathInput.length > 200 || /[\s?#]/.test(pathInput)) {
    throw Object.assign(new Error('Enter a WebSocket path starting with /, such as /mqtt.'), { statusCode: 400 });
  }
  if (deviceId.length > 128 || /[\s/# +]/.test(deviceId) || deviceId.includes('+') || deviceId.includes('#')) {
    throw Object.assign(new Error('Device ID must be one MQTT topic segment without spaces, slashes, +, or #.'), { statusCode: 400 });
  }
  const hostForUrl = hostInput.includes(':') && !hostInput.startsWith('[') ? `[${hostInput}]` : hostInput;
  let url;
  try {
    url = new URL(`wss://${hostForUrl}:${port}${pathInput}`);
  } catch {
    throw Object.assign(new Error('Enter a valid broker host name or IP address.'), { statusCode: 400 });
  }
  return { host: url.hostname.replace(/^\[|\]$/g, '').toLowerCase(), port, path: url.pathname, deviceId };
}

function brokerTopic(config, suffix) {
  return config.deviceId ? `${config.deviceId}/${suffix}` : '';
}

function observeBrokerMessage(config, topic, payload) {
  const value = payload.toString('utf8').trim();
  if (topic === brokerTopic(config, 'availability')) {
    brokerDeviceState.availability = value === 'online' || value === 'offline' ? value : 'unknown';
    brokerDeviceState.updatedAt = new Date().toISOString();
    return;
  }
  if (topic === brokerTopic(config, 'state')) {
    brokerDeviceState.state = value === 'ON' || value === 'OFF' ? value : 'unknown';
    brokerDeviceState.updatedAt = new Date().toISOString();
    return;
  }
  if (topic !== brokerTopic(config, 'result')) return;
  try {
    const result = JSON.parse(value);
    if (typeof result.command_id !== 'string' || !['COMPLETED', 'FAILED'].includes(result.status)) return;
    const command = pendingBrokerCommands.get(result.command_id);
    if (!command) return;
    brokerDeviceState.lastCommand = { ...command, status: result.status, completedAt: new Date().toISOString() };
    pendingBrokerCommands.delete(result.command_id);
  } catch {
    // Ignore malformed messages on the result topic.
  }
}

async function refreshBrokerSubscriptions(client, config) {
  const topics = [
    brokerTopic(config, 'availability'),
    brokerTopic(config, 'state'),
    brokerTopic(config, 'result'),
  ];
  if (brokerClient !== client || !brokerConnectionState.connected) {
    throw Object.assign(new Error('The MQTT connection is no longer active.'), { statusCode: 409 });
  }
  await new Promise((resolve, reject) => {
    client.unsubscribe(topics, (error) => error ? reject(error) : resolve());
  });
  const forcedTopics = Object.fromEntries(topics.map((topic) => [topic, { qos: 1 }]));
  forcedTopics.resubscribe = true;
  await new Promise((resolve, reject) => {
    client.subscribe(forcedTopics, { qos: 1 }, (error, granted = []) => {
      if (error) { reject(error); return; }
      if (granted.length !== topics.length) {
        reject(Object.assign(new Error('The broker did not confirm all device topic subscriptions.'), { statusCode: 502 }));
        return;
      }
      const rejected = granted.filter((subscription) => subscription.qos === 128).map((subscription) => subscription.topic);
      if (rejected.length) {
        reject(Object.assign(new Error(`Broker denied subscriptions: ${rejected.join(', ')}.`), { statusCode: 403 }));
        return;
      }
      resolve();
    });
  });
  if (brokerClient === client) {
    brokerConnectionState.subscriptionsReady = true;
    brokerConnectionState.lastError = '';
  }
}

function certificateBlocks(value, label) {
  if (typeof value !== 'string' || value.length > 100_000) {
    throw Object.assign(new Error(`${label} must be a PEM or base64-encoded X.509 certificate smaller than 100 KB.`), { statusCode: 400 });
  }
  let normalized = value.replace(/^\uFEFF/, '').replace(/\r\n?/g, '\n').trim();
  normalized = normalized
    .replace(/-----BEGIN X509 CERTIFICATE-----/gi, '-----BEGIN CERTIFICATE-----')
    .replace(/-----END X509 CERTIFICATE-----/gi, '-----END CERTIFICATE-----');
  let blocks = normalized.match(/-----BEGIN CERTIFICATE-----[\s\S]*?-----END CERTIFICATE-----/gi) || [];
  if (!blocks.length && /^[A-Za-z0-9+/=\s]+$/.test(normalized) && normalized.length > 128) {
    try {
      blocks = [new X509Certificate(Buffer.from(normalized.replace(/\s/g, ''), 'base64')).toString()];
    } catch {
      // Report the common X.509 format below.
    }
  }
  if (!blocks.length || blocks.length > 10) {
    throw Object.assign(new Error(`${label} must contain one to ten X.509 certificates in PEM or base64-encoded DER format.`), { statusCode: 400 });
  }
  try {
    blocks = blocks.map((block) => new X509Certificate(block).toString());
  } catch {
    throw Object.assign(new Error(`${label} is not a valid X.509 certificate. Use a PEM certificate beginning with -----BEGIN CERTIFICATE----- or upload a DER .cer/.der file.`), { statusCode: 400 });
  }
  return `${blocks.join('\n')}\n`;
}

async function generateBrokerCsr() {
  const { deviceId } = await readBrokerConfig();
  if (!deviceId || Buffer.byteLength(deviceId, 'utf8') > 64) {
    throw Object.assign(new Error('Save a Device ID of at most 64 bytes in MQTT settings before generating the CSR.'), { statusCode: 400 });
  }
  const keys = forge.pki.rsa.generateKeyPair({ bits: 2048, e: 0x10001 });
  const request = forge.pki.createCertificationRequest();
  request.publicKey = keys.publicKey;
  request.setSubject([
    { name: 'commonName', value: deviceId },
  ]);
  request.sign(keys.privateKey, forge.md.sha256.create());
  const pair = {
    privateKey: forge.pki.privateKeyToPem(keys.privateKey),
    csr: forge.pki.certificationRequestToPem(request),
  };
  await storePrivateFile(brokerKeyPairFile, JSON.stringify(pair));
  await unlink(brokerCertificateFile).catch((error) => { if (error.code !== 'ENOENT') throw error; });
  return pair.csr;
}

async function saveBrokerClientCertificate(value) {
  const normalized = certificateBlocks(value, 'the signed client certificate');
  const keyPair = await readBrokerKeyPair();
  if (!keyPair) throw Object.assign(new Error('Generate a CSR before adding the signed client certificate.'), { statusCode: 409 });
  try {
    const clientCertificate = new X509Certificate(
      normalized.match(/-----BEGIN CERTIFICATE-----[\s\S]*?-----END CERTIFICATE-----/)[0],
    );
    const privatePublicKey = createPublicKey(createPrivateKey(keyPair.privateKey)).export({ format: 'der', type: 'spki' });
    const certificatePublicKey = clientCertificate.publicKey.export({ format: 'der', type: 'spki' });
    if (privatePublicKey.length !== certificatePublicKey.length || !timingSafeEqual(privatePublicKey, certificatePublicKey)) {
      throw new Error('The certificate was not signed from this CSR.');
    }
    const now = new Date();
    if (now < new Date(clientCertificate.validFrom) || now > new Date(clientCertificate.validTo)) {
      throw new Error('The client certificate is not currently valid.');
    }
  } catch (error) {
    throw Object.assign(new Error(error.message || 'Could not validate the signed client certificate.'), { statusCode: 400 });
  }
  await storePrivateFile(brokerCertificateFile, normalized);
}

function brokerConnectionError(error) {
  const code = typeof error?.code === 'string' && /^[A-Z0-9_]+$/.test(error.code) ? ` (${error.code})` : '';
  return `Could not connect to the broker${code}. Check the host, port, path, certificate, and CA.`;
}

async function connectBroker(protocolVersion = 5) {
  if (brokerConnectionState.connected) return { ...brokerConnectionState };
  if (brokerConnectionState.connecting && brokerClient) return { ...brokerConnectionState };
  if (brokerClient) {
    const previousClient = brokerClient;
    brokerClient = null;
    previousClient.end(true);
  }
  const [config, keyPair, certificate, ca] = await Promise.all([
    readBrokerConfig(), readBrokerKeyPair(), readOptionalFile(brokerCertificateFile), readOptionalFile(brokerCaFile),
  ]);
  if (!config.host || !config.deviceId || !keyPair || !certificate || !ca) {
    throw Object.assign(new Error('Set the broker host and Device ID, generate a CSR, and provide the signed certificate and broker CA first.'), { statusCode: 409 });
  }
  const host = config.host.includes(':') ? `[${config.host}]` : config.host;
  const url = `wss://${host}:${config.port}${config.path}`;
  brokerConnectionState.connected = false;
  brokerConnectionState.connecting = true;
  brokerConnectionState.lastError = '';
  const client = mqtt.connect(url, {
    ca,
    cert: certificate,
    key: keyPair.privateKey,
    rejectUnauthorized: true,
    connectTimeout: 12_000,
    reconnectPeriod: 5_000,
    clean: true,
    protocolVersion,
    clientId: `autobootweb-${randomBytes(6).toString('hex')}`,
  });
  brokerClient = client;

  client.on('message', (topic, payload) => observeBrokerMessage(config, topic, payload));
  client.on('connect', () => {
    if (brokerClient !== client) return;
    brokerConnectionState.connected = true;
    brokerConnectionState.connecting = false;
    brokerConnectionState.handshakeSucceeded = true;
    brokerConnectionState.protocolVersion = protocolVersion;
    brokerConnectionState.lastHandshakeAt = new Date().toISOString();
    brokerConnectionState.lastError = '';
    brokerConnectionState.subscriptionsReady = false;
    client.subscribe([
      brokerTopic(config, 'availability'),
      brokerTopic(config, 'state'),
      brokerTopic(config, 'result'),
    ], { qos: 1 }, (error, granted = []) => {
      if (brokerClient !== client) return;
      const rejected = granted.filter((subscription) => subscription.qos === 128).map((subscription) => subscription.topic);
      if (error || rejected.length) {
        brokerConnectionState.subscriptionsReady = false;
        brokerConnectionState.lastError = error
          ? 'Connected to the broker, but could not subscribe to the device topics.'
          : `Broker denied subscriptions: ${rejected.join(', ')}.`;
        return;
      }
      brokerConnectionState.subscriptionsReady = true;
      brokerConnectionState.lastError = '';
    });
  });
  client.on('reconnect', () => {
    if (brokerClient !== client) return;
    brokerConnectionState.connected = false;
    brokerConnectionState.connecting = true;
    brokerConnectionState.subscriptionsReady = false;
  });
  client.on('offline', () => {
    if (brokerClient === client) brokerConnectionState.connected = false;
  });
  client.on('close', () => {
    if (brokerClient !== client) return;
    brokerConnectionState.connected = false;
    brokerConnectionState.connecting = false;
    brokerConnectionState.subscriptionsReady = false;
    if (!brokerConnectionState.handshakeSucceeded && !brokerConnectionState.lastError) {
      brokerConnectionState.lastError = brokerConnectionError({ code: 'CONNECTION_CLOSED' });
    }
  });
  client.on('error', (error) => {
    if (brokerClient !== client) return;
    if (protocolVersion === 5 && error?.code === 132) {
      brokerClient = null;
      brokerConnectionState.connected = false;
      brokerConnectionState.connecting = true;
      brokerConnectionState.protocolVersion = null;
      brokerConnectionState.subscriptionsReady = false;
      brokerConnectionState.lastError = '';
      client.end(true);
      void connectBroker(4).catch((fallbackError) => {
        brokerConnectionState.connected = false;
        brokerConnectionState.connecting = false;
        brokerConnectionState.lastError = brokerConnectionError(fallbackError);
      });
      return;
    }
    brokerConnectionState.connected = false;
    brokerConnectionState.connecting = false;
    brokerConnectionState.subscriptionsReady = false;
    brokerConnectionState.lastError = brokerConnectionError(error);
  });
  return { ...brokerConnectionState };
}

async function connectBrokerIfConfigured() {
  try {
    const [config, keyPair, certificate, ca] = await Promise.all([
      readBrokerConfig(), readBrokerKeyPair(), readOptionalFile(brokerCertificateFile), readOptionalFile(brokerCaFile),
    ]);
    if (!config.host || !config.deviceId || !keyPair || !certificate || !ca) return { ...brokerConnectionState };
    return await connectBroker();
  } catch (error) {
    brokerConnectionState.connected = false;
    brokerConnectionState.connecting = false;
    brokerConnectionState.subscriptionsReady = false;
    brokerConnectionState.lastError = brokerConnectionError(error);
    console.warn('Could not start the MQTT broker connection:', brokerConnectionState.lastError);
    return { ...brokerConnectionState };
  }
}

async function publishBrokerCommand(command) {
  if (!['get', 'start', 'restart'].includes(command)) {
    throw Object.assign(new Error('Unknown device command.'), { statusCode: 400 });
  }
  if (!brokerClient || !brokerConnectionState.connected) {
    throw Object.assign(new Error('Connect to the MQTT broker before controlling the device.'), { statusCode: 409 });
  }
  const config = await readBrokerConfig();
  if (!config.deviceId) throw Object.assign(new Error('Set the Device ID in MQTT settings first.'), { statusCode: 409 });
  if (command === 'get') await refreshBrokerSubscriptions(brokerClient, config);
  const commandId = randomUUID();
  const topic = brokerTopic(config, `command/${command}`);
  const commandInfo = { commandId, command, topic, status: 'PUBLISHING', sentAt: new Date().toISOString() };
  const payload = JSON.stringify({ command_id: commandId });
  pendingBrokerCommands.set(commandId, commandInfo);
  brokerDeviceState.lastCommand = commandInfo;
  const publishAck = await new Promise((resolve, reject) => {
    brokerClient.publish(topic, payload, { qos: 1, retain: false }, (error, packet) => error ? reject(error) : resolve(packet));
  }).catch((error) => {
    if (pendingBrokerCommands.has(commandId)) {
      pendingBrokerCommands.delete(commandId);
      brokerDeviceState.lastCommand = { ...commandInfo, status: 'FAILED' };
    }
    const rejectedByBroker = Number.isInteger(error?.code) && error.code >= 128;
    const message = rejectedByBroker
      ? `The broker rejected ${topic}: ${error.message}.`
      : 'Could not send the device command to the broker.';
    throw Object.assign(new Error(message), { statusCode: 502, cause: error });
  });
  if (publishAck?.reasonCode === 16) {
    pendingBrokerCommands.delete(commandId);
    brokerDeviceState.lastCommand = { ...commandInfo, status: 'NO_SUBSCRIBERS' };
    return brokerDeviceState.lastCommand;
  }
  if (pendingBrokerCommands.has(commandId)) {
    const acknowledgedCommand = { ...commandInfo, status: 'ACKNOWLEDGED' };
    pendingBrokerCommands.set(commandId, acknowledgedCommand);
    brokerDeviceState.lastCommand = acknowledgedCommand;
  }
  while (pendingBrokerCommands.size > 100) pendingBrokerCommands.delete(pendingBrokerCommands.keys().next().value);
  return brokerDeviceState.lastCommand?.commandId === commandId ? brokerDeviceState.lastCommand : commandInfo;
}

async function disconnectBroker() {
  const client = brokerClient;
  brokerClient = null;
  brokerConnectionState.connected = false;
  brokerConnectionState.connecting = false;
  brokerConnectionState.protocolVersion = null;
  brokerConnectionState.subscriptionsReady = false;
  if (!client) return { ...brokerConnectionState };
  await new Promise((resolve) => {
    const timer = setTimeout(resolve, 2_000);
    client.end(true, {}, () => { clearTimeout(timer); resolve(); });
  });
  return { ...brokerConnectionState };
}

async function resetBrokerConnection() {
  await disconnectBroker();
  brokerConnectionState.handshakeSucceeded = false;
  brokerConnectionState.subscriptionsReady = false;
  brokerConnectionState.lastHandshakeAt = null;
  brokerConnectionState.lastError = '';
  brokerDeviceState.availability = 'unknown';
  brokerDeviceState.state = 'unknown';
  brokerDeviceState.updatedAt = null;
  brokerDeviceState.lastCommand = null;
  pendingBrokerCommands.clear();
}

function rotateSessions(request, response, email) {
  const current = authSession(request);
  const duration = Math.max(1, (current?.expiresAt || Date.now() + sessionDuration) - Date.now());
  for (const [token, session] of sessions) {
    if (session.email === email) sessions.delete(token);
  }
  const token = randomBytes(32).toString('base64url');
  sessions.set(token, { email, expiresAt: Date.now() + duration });
  sendJson(response, 200, { ok: true, account: publicAccount(administrator) }, {
    'Set-Cookie': cookieHeader(token, duration),
  });
}

async function initializeSetupToken() {
  await mkdir(dataDirectory, { recursive: true, mode: 0o700 });
  await chmod(dataDirectory, 0o700);

  if (administrator) {
    await unlink(setupTokenFile).catch((error) => {
      if (error.code !== 'ENOENT') throw error;
    });
    return;
  }

  const token = randomBytes(32).toString('base64url');
  const temporaryFile = path.join(dataDirectory, `.setup-${randomBytes(8).toString('hex')}.tmp`);
  const file = await open(temporaryFile, 'wx', 0o600);
  try {
    await file.writeFile(`${token}\n`);
    await file.sync();
  } catch (error) {
    await file.close();
    await unlink(temporaryFile).catch(() => {});
    throw error;
  }
  await file.close();
  try {
    await link(temporaryFile, setupTokenFile);
  } catch (error) {
    if (error.code !== 'EEXIST') throw error;
    const existing = (await readFile(setupTokenFile, 'utf8')).trim();
    if (existing.length < 40) throw new Error('The setup token file is empty or invalid; remove it and restart the service.');
  } finally {
    await unlink(temporaryFile).catch(() => {});
  }
}

async function verifySetupToken(candidate) {
  if (typeof candidate !== 'string' || candidate.length > 256) return false;
  let stored;
  try {
    stored = (await readFile(setupTokenFile, 'utf8')).trim();
  } catch {
    return false;
  }
  const expected = Buffer.from(stored, 'utf8');
  const received = Buffer.from(candidate.trim(), 'utf8');
  return expected.length === received.length && expected.length >= 40 && timingSafeEqual(expected, received);
}

async function verifyPassword(password) {
  if (!administrator) return false;
  const salt = Buffer.from(administrator.salt, 'base64url');
  const expected = Buffer.from(administrator.passwordHash, 'base64url');
  if (salt.length !== 16 || expected.length !== 64) return false;
  const actual = Buffer.from(await scrypt(password, salt, expected.length, scryptOptions));
  return timingSafeEqual(actual, expected);
}

async function serveStatic(request, response, url) {
  if (!['GET', 'HEAD'].includes(request.method || '')) {
    response.writeHead(405, { Allow: 'GET, HEAD' });
    response.end();
    return;
  }

  let pathname;
  try {
    pathname = decodeURIComponent(url.pathname);
  } catch {
    response.writeHead(400);
    response.end();
    return;
  }

  const requested = pathname === '/' ? 'index.html' : pathname.replace(/^\/+/, '');
  let filePath = path.resolve(publicRoot, requested);
  if (!filePath.startsWith(publicRoot + path.sep) && filePath !== path.join(publicRoot, 'index.html')) {
    response.writeHead(404);
    response.end();
    return;
  }

  let fileInfo;
  try {
    fileInfo = await stat(filePath);
    if (fileInfo.isDirectory()) {
      filePath = path.join(filePath, 'index.html');
      fileInfo = await stat(filePath);
    }
  } catch {
    filePath = path.join(publicRoot, 'index.html');
    try {
      fileInfo = await stat(filePath);
    } catch {
      response.writeHead(503, { 'Content-Type': 'text/plain; charset=utf-8' });
      response.end('Build the site before starting the web server.');
      return;
    }
  }

  if (!fileInfo.isFile()) {
    response.writeHead(404);
    response.end();
    return;
  }

  response.writeHead(200, {
    'Cache-Control': path.extname(filePath) === '.html' ? 'no-cache' : 'public, max-age=3600',
    'Content-Length': fileInfo.size,
    'Content-Type': mimeTypes[path.extname(filePath).toLowerCase()] || 'application/octet-stream',
    'X-Content-Type-Options': 'nosniff',
  });
  if (request.method === 'HEAD') response.end();
  else createReadStream(filePath).pipe(response);
}

async function handleApi(request, response, url) {
  if (request.method === 'GET' && url.pathname === '/api/auth/status') {
    sendJson(response, 200, {
      setupRequired: !administrator,
      totpEnabled: Boolean(administrator?.totp),
      passkeyEnabled: Boolean(credentialsFor(administrator || {}).length),
    });
    return;
  }

  if (request.method === 'GET' && url.pathname === '/api/auth/session') {
    const session = currentSession(request);
    sendJson(response, 200, session
      ? { authenticated: true, email: session.email }
      : { authenticated: false });
    return;
  }

  if (['POST', 'DELETE'].includes(request.method || '') && url.pathname.startsWith('/api/auth/')) {
    if (!verifySameOrigin(request)) {
      sendJson(response, 403, { error: 'Request origin could not be verified.' });
      return;
    }
  }

  if (request.method === 'POST' && url.pathname === '/api/auth/setup') {
    if (isRateLimited(request)) {
      sendJson(response, 429, { error: 'Too many attempts. Try again in a few minutes.' }, { 'Retry-After': '300' });
      return;
    }
    if (administrator || setupInProgress) {
      sendJson(response, 409, { error: 'The administrator account has already been created.' });
      return;
    }

    const body = await readJsonBody(request);
    const email = normalizeEmail(body.email);
    const password = typeof body.password === 'string' ? body.password : '';
    if (!(await verifySetupToken(body.setupToken))) {
      sendJson(response, 403, { error: 'The one-time setup code is incorrect.' });
      return;
    }
    if (!validEmail(email)) {
      sendJson(response, 400, { error: 'Enter a username using 1–64 letters, numbers, dots, underscores or hyphens.' });
      return;
    }
    if (password.length < 12 || password.length > 1024) {
      sendJson(response, 400, { error: 'Use a password between 12 and 1024 characters.' });
      return;
    }

    setupInProgress = true;
    try {
      administrator = await persistAdministrator(email, password);
      await unlink(setupTokenFile).catch(() => {});
      const session = createSession(email, true);
      sendJson(response, 201, { authenticated: true, email }, {
        'Set-Cookie': cookieHeader(session.token, session.duration),
      });
    } catch (error) {
      if (error.code !== 'EEXIST') throw error;
      administrator = await readAdministratorFile();
      sendJson(response, 409, { error: 'The administrator account has already been created.' });
    } finally {
      setupInProgress = false;
    }
    return;
  }

  if (request.method === 'POST' && url.pathname === '/api/auth/login') {
    if (isRateLimited(request)) {
      sendJson(response, 429, { error: 'Too many attempts. Try again in a few minutes.' }, { 'Retry-After': '300' });
      return;
    }
    if (!administrator) {
      sendJson(response, 409, { error: 'Set up the first administrator account before signing in.' });
      return;
    }

    const body = await readJsonBody(request);
    const email = normalizeEmail(body.email);
    const password = typeof body.password === 'string' ? body.password : '';
    if (password.length > 1024) {
      sendJson(response, 400, { error: 'Password must not exceed 1024 characters.' });
      return;
    }
    if (passwordChecksInProgress >= maxConcurrentPasswordChecks) {
      sendJson(response, 429, { error: 'The sign-in service is busy. Try again shortly.' }, { 'Retry-After': '5' });
      return;
    }
    passwordChecksInProgress += 1;
    let passwordMatches;
    try {
      passwordMatches = await verifyPassword(password);
    } finally {
      passwordChecksInProgress -= 1;
    }
    if (email !== administrator.email || !passwordMatches) {
      sendJson(response, 401, { error: 'The username or password is incorrect.' });
      return;
    }

    if (administrator.totp) {
      const step = verifyTotp(administrator.totp, body.totp);
      if (step === null) {
        sendJson(response, 401, { error: 'Enter a valid unused authenticator code.' });
        return;
      }
      administrator.totp.lastStep = step;
      await saveAdministrator(administrator);
    }

    const remember = body.remember === true;
    const session = createSession(email, remember);
    sendJson(response, 200, { authenticated: true, email }, {
      'Set-Cookie': cookieHeader(session.token, session.duration),
    });
    return;
  }

  if (request.method === 'POST' && url.pathname === '/api/auth/logout') {
    const token = readCookie(request, sessionCookie);
    sessions.delete(token);
    sendJson(response, 200, { authenticated: false }, { 'Set-Cookie': clearCookieHeader() });
    return;
  }

  if (request.method === 'GET' && url.pathname === '/api/auth/account') {
    if (!authSession(request)) {
      sendJson(response, 401, { error: 'Sign in to manage account security.' });
      return;
    }
    sendJson(response, 200, { account: publicAccount(administrator) });
    return;
  }

  if (request.method === 'GET' && url.pathname === '/api/auth/broker') {
    if (!authSession(request)) { sendJson(response, 401, { error: 'Sign in to manage broker settings.' }); return; }
    sendJson(response, 200, await brokerPublicState());
    return;
  }

  if (request.method === 'GET' && url.pathname === '/api/auth/broker/status') {
    if (!authSession(request)) { sendJson(response, 401, { error: 'Sign in to view broker status.' }); return; }
    sendJson(response, 200, { connection: { ...brokerConnectionState }, device: { ...brokerDeviceState } });
    return;
  }

  if (request.method === 'POST' && url.pathname === '/api/auth/broker/config') {
    if (!authSession(request)) { sendJson(response, 401, { error: 'Sign in to manage broker settings.' }); return; }
    const body = await readJsonBody(request);
    const config = normalizeBrokerConfig(body);
    await storePrivateFile(brokerConfigFile, JSON.stringify(config));
    await resetBrokerConnection();
    await connectBrokerIfConfigured();
    sendJson(response, 200, { config });
    return;
  }

  if (request.method === 'POST' && url.pathname === '/api/auth/broker/csr') {
    if (isRateLimited(request)) { sendJson(response, 429, { error: 'Too many requests. Try again in a few minutes.' }); return; }
    if (!authSession(request)) { sendJson(response, 401, { error: 'Sign in to generate a client CSR.' }); return; }
    const body = await readJsonBody(request);
    if (await readBrokerKeyPair()) {
      if (body.replaceExisting !== true) { sendJson(response, 409, { error: 'A CSR already exists. Confirm that you want to replace it.' }); return; }
      if (!(await verifyCurrentCredentials(request, body, 'generate-broker-csr'))) {
        sendJson(response, 401, { error: 'Your current password and an active security factor could not be verified.' });
        return;
      }
    }
    const csr = await generateBrokerCsr();
    await resetBrokerConnection();
    await connectBrokerIfConfigured();
    sendJson(response, 201, { csr });
    return;
  }

  if (request.method === 'POST' && url.pathname === '/api/auth/broker/client-certificate') {
    if (!authSession(request)) { sendJson(response, 401, { error: 'Sign in to install the signed client certificate.' }); return; }
    const body = await readJsonBody(request);
    await saveBrokerClientCertificate(body.pem);
    await resetBrokerConnection();
    await connectBrokerIfConfigured();
    sendJson(response, 200, await brokerPublicState());
    return;
  }

  if (request.method === 'POST' && url.pathname === '/api/auth/broker/ca') {
    if (!authSession(request)) { sendJson(response, 401, { error: 'Sign in to install the broker CA.' }); return; }
    const body = await readJsonBody(request);
    const ca = certificateBlocks(body.pem, 'the broker CA');
    await storePrivateFile(brokerCaFile, ca);
    await resetBrokerConnection();
    await connectBrokerIfConfigured();
    sendJson(response, 200, await brokerPublicState());
    return;
  }

  if (request.method === 'POST' && url.pathname === '/api/auth/broker/connect') {
    if (isRateLimited(request)) { sendJson(response, 429, { error: 'Too many connection attempts. Try again in a few minutes.' }); return; }
    if (!authSession(request)) { sendJson(response, 401, { error: 'Sign in to test the broker connection.' }); return; }
    sendJson(response, 200, { connection: await connectBroker() });
    return;
  }

  if (request.method === 'POST' && url.pathname === '/api/auth/broker/disconnect') {
    if (!authSession(request)) { sendJson(response, 401, { error: 'Sign in to disconnect from the broker.' }); return; }
    sendJson(response, 200, { connection: await disconnectBroker() });
    return;
  }

  if (request.method === 'POST' && url.pathname === '/api/auth/broker/command') {
    if (!authSession(request)) { sendJson(response, 401, { error: 'Sign in to control the device.' }); return; }
    const body = await readJsonBody(request);
    const command = await publishBrokerCommand(body.command);
    sendJson(response, 202, { command });
    return;
  }

  if (request.method === 'POST' && url.pathname === '/api/auth/reauth/options') {
    if (isRateLimited(request)) { sendJson(response, 429, { error: 'Too many attempts. Try again in a few minutes.' }); return; }
    const session = authSession(request);
    if (!session) { sendJson(response, 401, { error: 'Sign in to verify your identity.' }); return; }
    const body = await readJsonBody(request);
    const action = typeof body.action === 'string' ? body.action : '';
    const allowed = ['change-password', 'add-passkey', 'enable-totp', 'disable-totp', 'generate-broker-csr'];
    const removal = action.startsWith('remove-passkey:')
      ? action.slice('remove-passkey:'.length)
      : '';
    if (!allowed.includes(action) && (!removal || !credentialsFor(administrator).some((entry) => entry.id === removal))) {
      sendJson(response, 400, { error: 'Unknown security operation.' });
      return;
    }
    if (!credentialsFor(administrator).length) {
      sendJson(response, 409, { error: 'No passkey is registered for passkey verification.' });
      return;
    }
    const context = rpContext(request);
    const options = await generateAuthenticationOptions({
      rpID: context.rpID,
      userVerification: 'required',
      allowCredentials: credentialsFor(administrator).map((credential) => ({
        id: credential.id,
        type: 'public-key',
        transports: credential.transports,
      })),
    });
    passkeyChallenges.set(`reauth:${session.token}:${action}`, {
      challenge: options.challenge,
      ...context,
      expiresAt: Date.now() + 5 * 60_000,
    });
    sendJson(response, 200, options);
    return;
  }

  if (request.method === 'POST' && url.pathname === '/api/auth/password/change') {
    if (isRateLimited(request)) {
      sendJson(response, 429, { error: 'Too many attempts. Try again in a few minutes.' }, { 'Retry-After': '300' });
      return;
    }
    const session = authSession(request);
    if (!session) { sendJson(response, 401, { error: 'Sign in to change your password.' }); return; }
    const body = await readJsonBody(request);
    const nextPassword = typeof body.newPassword === 'string' ? body.newPassword : '';
    if (nextPassword.length < 12 || nextPassword.length > 1024) {
      sendJson(response, 400, { error: 'Use a password between 12 and 1024 characters.' });
      return;
    }
    if (!(await verifyCurrentCredentials(request, body, 'change-password'))) {
      sendJson(response, 401, { error: 'Your current password and an active security factor could not be verified.' });
      return;
    }
    const salt = randomBytes(16);
    const passwordHash = await scrypt(nextPassword, salt, 64, scryptOptions);
    await saveAdministrator({ ...administrator, salt: salt.toString('base64url'), passwordHash: Buffer.from(passwordHash).toString('base64url') });
    rotateSessions(request, response, administrator.email);
    return;
  }

  if (request.method === 'POST' && url.pathname === '/api/auth/totp/setup') {
    if (isRateLimited(request)) { sendJson(response, 429, { error: 'Too many attempts. Try again in a few minutes.' }); return; }
    const session = authSession(request);
    if (!session) { sendJson(response, 401, { error: 'Sign in to enable an authenticator.' }); return; }
    if (administrator.totp) { sendJson(response, 409, { error: 'The authenticator is already enabled.' }); return; }
    const body = await readJsonBody(request);
    if (!(await verifyCurrentCredentials(request, body, 'enable-totp'))) { sendJson(response, 401, { error: 'Your current password and an active security factor could not be verified.' }); return; }
    const totp = new OTPAuth.TOTP({
      issuer: 'AutoBootWeb',
      label: administrator.email,
      algorithm: 'SHA1',
      digits: 6,
      period: 30,
      secret: new OTPAuth.Secret({ size: 20 }),
    });
    const secret = totp.secret.base32;
    const key = session.token;
    pendingTotp.set(key, { encrypted: encryptTotpSecret(secret), secret, expiresAt: Date.now() + 10 * 60_000 });
    sendJson(response, 200, { secret, qr: await QRCode.toDataURL(totp.toString(), { width: 240, margin: 2 }), expiresIn: 600 });
    return;
  }

  if (request.method === 'POST' && url.pathname === '/api/auth/totp/verify') {
    if (isRateLimited(request)) { sendJson(response, 429, { error: 'Too many attempts. Try again in a few minutes.' }); return; }
    const session = authSession(request);
    if (!session) { sendJson(response, 401, { error: 'Sign in to verify the authenticator.' }); return; }
    const body = await readJsonBody(request);
    const pending = pendingTotp.get(session.token);
    if (!pending || pending.expiresAt <= Date.now()) {
      pendingTotp.delete(session.token);
      sendJson(response, 400, { error: 'The authenticator setup expired. Start again.' });
      return;
    }
    const step = verifyTotp(pending, body.code);
    if (step === null) { sendJson(response, 400, { error: 'Enter the current six-digit authenticator code.' }); return; }
    await saveAdministrator({ ...administrator, totp: { encrypted: pending.encrypted, lastStep: step } });
    pendingTotp.delete(session.token);
    sendJson(response, 200, { ok: true, account: publicAccount(administrator) });
    return;
  }

  if (request.method === 'DELETE' && url.pathname === '/api/auth/totp') {
    if (isRateLimited(request)) { sendJson(response, 429, { error: 'Too many attempts. Try again in a few minutes.' }); return; }
    const session = authSession(request);
    if (!session) { sendJson(response, 401, { error: 'Sign in to disable the authenticator.' }); return; }
    const body = await readJsonBody(request);
    if (!administrator.totp || !(await verifyCurrentCredentials(request, body, 'disable-totp'))) {
      sendJson(response, 401, { error: 'Your current password and an active security factor could not be verified.' });
      return;
    }
    const updated = { ...administrator, totp: null };
    await saveAdministrator(updated);
    rotateSessions(request, response, updated.email);
    return;
  }

  if (request.method === 'POST' && url.pathname === '/api/auth/passkeys/options') {
    if (isRateLimited(request)) { sendJson(response, 429, { error: 'Too many attempts. Try again in a few minutes.' }); return; }
    const session = authSession(request);
    if (!session) { sendJson(response, 401, { error: 'Sign in to add a passkey.' }); return; }
    const context = rpContext(request);
    const body = await readJsonBody(request);
    if (!(await verifyCurrentCredentials(request, body, 'add-passkey'))) { sendJson(response, 401, { error: 'Your current password and an active security factor could not be verified.' }); return; }
    if (!administrator.webauthnUserId) {
      administrator.webauthnUserId = randomBytes(32).toString('base64url');
      await saveAdministrator(administrator);
    }
    const options = await generateRegistrationOptions({
      rpName: 'AutoBootWeb',
      rpID: context.rpID,
      userID: new Uint8Array(Buffer.from(administrator.webauthnUserId, 'base64url')),
      userName: administrator.email,
      userDisplayName: administrator.email,
      attestationType: 'none',
      authenticatorSelection: { residentKey: 'preferred', userVerification: 'required' },
      excludeCredentials: credentialsFor(administrator).map((credential) => ({ id: credential.id, type: 'public-key' })),
    });
    passkeyChallenges.set(`register:${session.token}`, { challenge: options.challenge, ...context, expiresAt: Date.now() + 5 * 60_000 });
    sendJson(response, 200, options);
    return;
  }

  if (request.method === 'POST' && url.pathname === '/api/auth/passkeys/verify') {
    if (isRateLimited(request)) { sendJson(response, 429, { error: 'Too many attempts. Try again in a few minutes.' }); return; }
    const session = authSession(request);
    if (!session) { sendJson(response, 401, { error: 'Sign in to add a passkey.' }); return; }
    const context = rpContext(request);
    const challengeKey = `register:${session.token}`;
    const saved = passkeyChallenges.get(challengeKey);
    passkeyChallenges.delete(challengeKey);
    if (!saved || saved.expiresAt <= Date.now()) { sendJson(response, 400, { error: 'The passkey request expired. Start again.' }); return; }
    const body = await readJsonBody(request);
    const verification = await verifyRegistrationResponse({
      response: body.credential,
      expectedChallenge: saved.challenge,
      expectedOrigin: context.origin,
      expectedRPID: context.rpID,
      requireUserVerification: true,
    });
    if (!verification.verified || !verification.registrationInfo) { sendJson(response, 400, { error: 'Passkey verification failed.' }); return; }
    const credentialId = body.credential?.id;
    if (!credentialId || credentialsFor(administrator).some((credential) => credential.id === credentialId)) {
      sendJson(response, 409, { error: 'This passkey is already registered.' });
      return;
    }
    const credential = verification.registrationInfo.credential;
    administrator.passkeys.push({
      id: credentialId,
      publicKey: Buffer.from(credential.publicKey).toString('base64url'),
      counter: credential.counter,
      transports: body.credential.response?.transports || [],
      name: typeof body.name === 'string' && body.name.trim() ? body.name.trim().slice(0, 64) : 'Passkey',
      createdAt: new Date().toISOString(),
    });
    await saveAdministrator(administrator);
    sendJson(response, 200, { account: publicAccount(administrator) });
    return;
  }

  if (request.method === 'DELETE' && url.pathname.startsWith('/api/auth/passkeys/')) {
    if (isRateLimited(request)) { sendJson(response, 429, { error: 'Too many attempts. Try again in a few minutes.' }); return; }
    const session = authSession(request);
    if (!session) { sendJson(response, 401, { error: 'Sign in to remove a passkey.' }); return; }
    const credentialId = decodeURIComponent(url.pathname.slice('/api/auth/passkeys/'.length));
    const body = await readJsonBody(request);
    if (!(await verifyCurrentCredentials(request, body, `remove-passkey:${credentialId}`))) { sendJson(response, 401, { error: 'Your current password and an active security factor could not be verified.' }); return; }
    const passkeys = credentialsFor(administrator);
    const nextPasskeys = passkeys.filter((credential) => credential.id !== credentialId);
    if (nextPasskeys.length === passkeys.length) { sendJson(response, 404, { error: 'Passkey not found.' }); return; }
    await saveAdministrator({ ...administrator, passkeys: nextPasskeys });
    rotateSessions(request, response, administrator.email);
    return;
  }

  if (request.method === 'POST' && url.pathname === '/api/auth/passkey/login/options') {
    if (isRateLimited(request)) { sendJson(response, 429, { error: 'Too many attempts. Try again in a few minutes.' }); return; }
    const body = await readJsonBody(request);
    const email = normalizeEmail(body.email);
    if (!administrator || email !== administrator.email || !credentialsFor(administrator).length) {
      sendJson(response, 401, { error: 'No passkey is available for this account.' }); return;
    }
    const context = rpContext(request);
    const options = await generateAuthenticationOptions({
      rpID: context.rpID,
      userVerification: 'required',
      allowCredentials: credentialsFor(administrator).map((credential) => ({ id: credential.id, type: 'public-key', transports: credential.transports })),
    });
    passkeyChallenges.set(`login:${email}`, { challenge: options.challenge, ...context, expiresAt: Date.now() + 5 * 60_000 });
    sendJson(response, 200, options);
    return;
  }

  if (request.method === 'POST' && url.pathname === '/api/auth/passkey/login/verify') {
    if (isRateLimited(request)) { sendJson(response, 429, { error: 'Too many attempts. Try again in a few minutes.' }); return; }
    const body = await readJsonBody(request);
    const email = normalizeEmail(body.email);
    const challengeKey = `login:${email}`;
    const saved = passkeyChallenges.get(challengeKey);
    passkeyChallenges.delete(challengeKey);
    if (!administrator || email !== administrator.email || !saved || saved.expiresAt <= Date.now()) {
      sendJson(response, 401, { error: 'The passkey request expired or could not be verified.' }); return;
    }
    const credential = credentialsFor(administrator).find((entry) => entry.id === body.credential?.id);
    if (!credential) { sendJson(response, 401, { error: 'This passkey is not registered.' }); return; }
    const context = rpContext(request);
    const verification = await verifyAuthenticationResponse({
      response: body.credential,
      expectedChallenge: saved.challenge,
      expectedOrigin: context.origin,
      expectedRPID: context.rpID,
      requireUserVerification: true,
      credential: {
        id: credential.id,
        publicKey: new Uint8Array(Buffer.from(credential.publicKey, 'base64url')),
        counter: credential.counter || 0,
        transports: credential.transports,
      },
    });
    if (!verification.verified) { sendJson(response, 401, { error: 'Passkey verification failed.' }); return; }
    credential.counter = verification.authenticationInfo.newCounter;
    await saveAdministrator(administrator);
    const session = createSession(email, true);
    sendJson(response, 200, { authenticated: true, email }, { 'Set-Cookie': cookieHeader(session.token, session.duration) });
    return;
  }

  sendJson(response, 404, { error: 'Not found.' });
}

async function readAdministratorFile() {
  try {
    const stored = JSON.parse(await readFile(accountFile, 'utf8'));
    if (typeof stored.email !== 'string' || typeof stored.salt !== 'string' || typeof stored.passwordHash !== 'string') {
      throw new Error('Administrator file has an invalid format.');
    }
    return stored;
  } catch (error) {
    if (error.code === 'ENOENT') return null;
    throw error;
  }
}

async function loadAdministrator() {
  administrator = await readAdministratorFile();
}

await loadAdministrator();
await initializeSetupToken();
totpEncryptionKey = await initializeTotpEncryptionKey();
const server = createServer((request, response) => {
  response.setHeader('Referrer-Policy', 'same-origin');
  const url = new URL(request.url || '/', 'http://localhost');
  const operation = url.pathname.startsWith('/api/')
    ? handleApi(request, response, url)
    : apiOnly
      ? Promise.resolve(sendJson(response, 404, { error: 'Not found.' }))
      : serveStatic(request, response, url);
  operation.catch((error) => {
    if (response.headersSent) {
      response.destroy(error);
      return;
    }
    const statusCode = Number.isInteger(error.statusCode) ? error.statusCode : 500;
    sendJson(response, statusCode, {
      error: statusCode === 500 ? 'The request could not be completed.' : error.message,
    });
  });
});

const host = process.env.HOST || '127.0.0.1';
server.listen(port, host, () => {
  console.log(`AutoBootWeb listening on http://${host}:${port}`);
  void connectBrokerIfConfigured();
});

for (const signal of ['SIGINT', 'SIGTERM']) {
  process.on(signal, () => server.close(() => process.exit(0)));
}
