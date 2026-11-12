// Web Serial protocol for Zephyr's AutoBoot USB maintenance interface.
const encoder = new TextEncoder();
const decoder = new TextDecoder();
const MAX_BUFFER = 65536;
const TIMEOUT = 12000;

export class AutoBootSerial {
  constructor(onChange = () => {}) {
    this.onChange = onChange;
    this.state = 'disconnected';
    this.port = null;
    this.reader = null;
    this.writer = null;
    this.buffer = new Uint8Array();
    this.waiter = null;
    this.failure = null;
    this.closing = false;
    this.pumpPromise = null;
    this.busy = false;
    this.generation = 0;
    this.onDisconnect = (event) => {
      if (event.target === this.port) this.fail(new Error('The USB device was disconnected.'));
    };
    globalThis.navigator?.serial?.addEventListener('disconnect', this.onDisconnect);
  }

  setState(state, message = '') { this.state = state; this.onChange(state, message); }
  static supported() { return typeof navigator !== 'undefined' && !!navigator.serial && globalThis.isSecureContext; }
  static async ports() { return navigator.serial.getPorts(); }
  describe(port = this.port) {
    const info = port?.getInfo?.() || {};
    const hex = value => value === undefined ? 'unknown' : `0x${value.toString(16).padStart(4, '0')}`;
    return `USB serial · vendor ${hex(info.usbVendorId)} · product ${hex(info.usbProductId)}`;
  }

  async connect(port) {
    if (this.state !== 'disconnected' && this.state !== 'error') throw new Error('Disconnect the current port first.');
    this.setState('connecting');
    this.failure = null;
    this.closing = false;
    this.port = port;
    const generation = ++this.generation;
    try {
      await port.open({ baudRate: 115200 });
      if (generation !== this.generation) { await port.close(); throw new Error('Connection cancelled.'); }
      if (port.setSignals) {
        try { await port.setSignals({ dataTerminalReady: true, requestToSend: true }); }
        catch (error) { if (error?.name !== 'NotSupportedError') throw error; }
      }
      if (generation !== this.generation) { await port.close(); throw new Error('Connection cancelled.'); }
      if (!port.readable || !port.writable) throw new Error('The serial port is not readable and writable.');
      this.reader = port.readable.getReader();
      this.writer = port.writable.getWriter();
      this.pumpPromise = this.pump();
      this.setState('connected');
    } catch (error) {
      await this.close();
      this.setState('error', error.message || 'Could not open the serial port.');
      throw error;
    }
  }

  async pump() {
    try {
      while (!this.closing) {
        const { value, done } = await this.reader.read();
        if (done) { if (!this.closing) throw new Error('The serial port closed.'); break; }
        if (!value?.length) continue;
        if (this.buffer.length + value.length > MAX_BUFFER) throw new Error('Serial input exceeded the buffer limit.');
        const next = new Uint8Array(this.buffer.length + value.length);
        next.set(this.buffer); next.set(value, this.buffer.length); this.buffer = next;
        this.wake();
      }
    } catch (error) { if (!this.closing) this.fail(error); }
  }
  wake() { if (this.waiter) { const wake = this.waiter; this.waiter = null; wake(); } }
  fail(error) {
    if (this.closing || this.failure) return;
    this.failure = error instanceof Error ? error : new Error('Serial communication failed.');
    this.wake();
    this.setState('error', this.failure.message);
    void this.close(true);
  }
  async close(keepError = false) {
    if (this.closing) return;
    this.generation++;
    this.closing = true;
    const port = this.port, reader = this.reader, writer = this.writer;
    this.failure ||= new Error('The serial connection was closed.');
    this.wake();
    try { await reader?.cancel(); } catch {}
    try { await this.pumpPromise; } catch {}
    try { reader?.releaseLock(); } catch {}
    try { writer?.releaseLock(); } catch {}
    try { await port?.close(); } catch {}
    this.reader = this.writer = this.port = this.pumpPromise = null;
    this.buffer = new Uint8Array();
    this.busy = false;
    this.closing = false;
    if (!keepError) this.setState('disconnected');
  }
  async dispose() {
    globalThis.navigator?.serial?.removeEventListener('disconnect', this.onDisconnect);
    await this.close();
  }
  async wait(deadline) {
    if (this.failure) throw this.failure;
    const left = deadline - Date.now();
    if (left <= 0) throw new Error('Timed out waiting for the device response.');
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => { this.waiter = null; reject(new Error('Timed out waiting for the device response.')); }, left);
      this.waiter = () => { clearTimeout(timer); resolve(); };
    });
    if (this.failure) throw this.failure;
  }
  async line(deadline) {
    while (true) {
      const end = this.buffer.indexOf(10);
      if (end !== -1) {
        const raw = this.buffer.slice(0, end); this.buffer = this.buffer.slice(end + 1);
        if (raw.length > 8192) throw new Error('Serial response line is too long.');
        return decoder.decode(raw).replace(/\r$/, '');
      }
      if (this.buffer.length > 8192) throw new Error('Serial response line is too long.');
      await this.wait(deadline);
    }
  }
  async bytes(count, deadline) {
    while (this.buffer.length < count) await this.wait(deadline);
    const result = this.buffer.slice(0, count); this.buffer = this.buffer.slice(count); return result;
  }
  async response(match, deadline, label) {
    while (true) {
      const raw = await this.line(deadline);
      // A partial diagnostic line can precede the protocol reply without a newline.
      const marker = raw.search(/BEGIN CSR|END CSR|ERR |READY|OK /);
      const line = marker < 0 ? raw : raw.slice(marker);
      if (line.startsWith('ERR ')) throw new Error(`Device error: ${line}`);
      if (match(line)) return line;
      if (/^(OK |READY|BEGIN CSR|END CSR)/.test(line)) throw new Error(`Unexpected ${label} response: ${line}`);
      // Ignore bounded diagnostic lines until the expected response arrives.
    }
  }
  async write(bytes, deadline) {
    if (!this.writer || this.failure) throw this.failure || new Error('Connect a USB device first.');
    const left = deadline - Date.now();
    if (left <= 0) throw new Error('Timed out writing to the device.');
    let timer;
    try {
      await Promise.race([
        this.writer.write(bytes),
        new Promise((_, reject) => { timer = setTimeout(() => reject(new Error('Timed out writing to the device.')), left); }),
      ]);
    } finally { clearTimeout(timer); }
  }
  async operation(action) {
    if (this.busy) throw new Error('Another USB operation is in progress.');
    if (!this.port || this.state !== 'connected') throw new Error('Connect a USB device first.');
    this.busy = true; this.setState('busy');
    try { return await action(Date.now() + TIMEOUT); }
    catch (error) {
      // A timeout or malformed response may leave a late reply in the stream.
      this.setState('error', error.message || 'USB operation failed.');
      await this.close(true);
      throw error;
    } finally { this.busy = false; if (this.port && !this.failure) this.setState('connected'); }
  }
  async command(text, expected) {
    return this.operation(async deadline => {
      const bytes = encoder.encode(`${text}\n`);
      if (bytes.length > 127) throw new Error('Command exceeds the firmware limit of 127 bytes.');
      await this.write(bytes, deadline);
      return this.response(line => line === expected, deadline, 'command');
    });
  }
  async csr() {
    return this.operation(async deadline => {
      await this.write(encoder.encode('CSR\n'), deadline);
      let header;
      try { header = await this.response(line => line.startsWith('BEGIN CSR'), deadline, 'CSR header'); }
      catch (error) { throw new Error(`CSR header: ${error.message}`); }
      const match = /^BEGIN CSR ([1-9][0-9]*)$/.exec(header);
      if (!match || Number(match[1]) > 4096) throw new Error('Invalid CSR header or size.');
      let body, terminator;
      try { body = await this.bytes(Number(match[1]), deadline); }
      catch (error) { throw new Error(`CSR body: ${error.message}`); }
      try { terminator = await this.line(deadline); }
      catch (error) { throw new Error(`CSR terminator: ${error.message}`); }
      if (terminator !== 'END CSR') throw new Error('Invalid CSR terminator.');
      const pem = new TextDecoder('utf-8', { fatal: true }).decode(body);
      if (!/^-----BEGIN CERTIFICATE REQUEST-----\r?\n[\s\S]*-----END CERTIFICATE REQUEST-----\r?\n?$/.test(pem)) throw new Error('Invalid CSR PEM body.');
      return pem;
    });
  }
  async upload(kind, bytes, progress = () => {}) {
    if (!['CLIENT', 'CA'].includes(kind) || !(bytes instanceof Uint8Array) || !bytes.length || bytes.length > 4096) throw new Error('Certificate must contain 1 to 4096 bytes.');
    return this.operation(async deadline => {
      progress('Waiting for device readiness…');
      await this.write(encoder.encode(`PUT ${kind} ${bytes.length}\n`), deadline);
      await this.response(line => line === 'READY', deadline, 'upload readiness');
      progress(`Sending ${bytes.length} bytes…`);
      await this.write(bytes, deadline);
      progress('Waiting for device confirmation…');
      await this.response(line => line === 'OK stored', deadline, 'upload');
      progress('');
    });
  }
}
