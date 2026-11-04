import { createConnection } from 'node:net';
import { lstat } from 'node:fs/promises';
import { dirname, isAbsolute, join } from 'node:path';
import { tmpdir, userInfo } from 'node:os';
import { pathToFileURL } from 'node:url';

export const ACTION = 'com.amazingpvper.relaymic.toggle';
export const STATES = Object.freeze({ on: 0, muted: 1, offline: 2, deafened: 3, ready: 4 });
const STATUS_FIELDS = ['running', 'voiceConnected', 'muted', 'deafened', 'discordConnected'];
const MAX_RESPONSE = 4096;

async function privateDirectory(path) {
  if (!isAbsolute(path)) return false;
  try {
    const entry = await lstat(path);
    return entry.isDirectory() && !entry.isSymbolicLink()
      && entry.uid === process.geteuid() && (entry.mode & 0o077) === 0;
  } catch { return false; }
}

async function defaultEndpoint() {
  // Match Qt's runtime directory, then ControlServer's private fallback.
  const runtime = process.env.XDG_RUNTIME_DIR || join(tmpdir(), `runtime-${userInfo().username}`);
  if (await privateDirectory(runtime)) return join(runtime, 'relay-control-v1.sock');
  const fallback = join(tmpdir(), `relay-control-${process.geteuid()}`);
  if (await privateDirectory(fallback)) return join(fallback, 'relay-control-v1.sock');
  throw new Error('Relay controls are unavailable');
}

export function stateFor(status) {
  if (!status || status.running !== true) return STATES.offline;
  if (status.muted) return STATES.muted;
  if (status.deafened) return STATES.deafened;
  return status.voiceConnected ? STATES.on : STATES.ready;
}

export class RelayControl {
  constructor({ endpoint, timeoutMs = 450 } = {}) {
    this.endpoint = endpoint;
    this.timeoutMs = timeoutMs;
  }

  async request(action) {
    if (action !== 'status' && action !== 'toggle-mute') throw new Error('Unsupported control action');
    const deadline = performance.now() + this.timeoutMs;
    const endpoint = this.endpoint || await defaultEndpoint();
    if (!isAbsolute(endpoint) || !await privateDirectory(dirname(endpoint))) throw new Error('Relay controls are unavailable');
    const entry = await lstat(endpoint);
    if (!entry.isSocket() || entry.isSymbolicLink() || entry.uid !== process.geteuid()) throw new Error('Relay controls are unavailable');
    const remaining = deadline - performance.now();
    if (remaining <= 0) throw new Error('Relay did not confirm the action');
    return new Promise((resolve, reject) => {
      const socket = createConnection({ path: endpoint });
      let response = Buffer.alloc(0);
      let finished = false;
      const finish = (error, status) => {
        if (finished) return;
        finished = true;
        clearTimeout(timer);
        socket.destroy();
        if (error) reject(error); else resolve(status);
      };
      const timer = setTimeout(() => finish(new Error('Relay did not confirm the action')), remaining);
      socket.once('connect', () => {
        if (performance.now() >= deadline) { finish(new Error('Relay did not confirm the action')); return; }
        // An ambiguous toggle is never retried. A later status read is safe.
        socket.write(JSON.stringify({ version: 1, action }) + '\n');
      });
      socket.on('data', chunk => {
        if (finished) return;
        if (response.length + chunk.length > MAX_RESPONSE) { finish(new Error('Invalid control response')); return; }
        response = Buffer.concat([response, chunk]);
        const newline = response.indexOf(10);
        if (newline < 0) return;
        try {
          if (response.subarray(newline + 1).toString('utf8').trim()) throw new Error();
          const parsed = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(response.subarray(0, newline)));
          const status = parsed?.status;
          if (parsed?.ok !== true || !status || Array.isArray(status)
            || STATUS_FIELDS.some(field => typeof status[field] !== 'boolean') || !status.running) throw new Error();
          finish(null, Object.fromEntries(STATUS_FIELDS.map(field => [field, status[field]])));
        } catch { finish(new Error('Invalid control response')); }
      });
      socket.once('error', () => finish(new Error('Relay controls are unavailable')));
      socket.once('end', () => { if (!finished) finish(new Error('Relay did not confirm the action')); });
      socket.once('close', () => { if (!finished) finish(new Error('Relay did not confirm the action')); });
    });
  }
}

export class RelayMicPlugin {
  constructor({ port, uuid, control = new RelayControl(), pollMs = 500, reconnectMs = 2000 }) {
    this.port = port;
    this.uuid = uuid;
    this.control = control;
    this.pollMs = pollMs;
    this.reconnectMs = reconnectMs;
    this.contexts = new Map();
    this.pressed = new Set();
    this.toggles = [];
    this.status = null;
    this.statusPending = false;
    this.busy = false;
    this.stopping = false;
    this.generation = 0;
  }

  send(event) {
    if (this.socket?.readyState === WebSocket.OPEN) this.socket.send(JSON.stringify(event));
  }

  publish(context, force = false) {
    if (!this.contexts.has(context)) return;
    const state = stateFor(this.status);
    if (force || this.contexts.get(context) !== state) {
      this.send({ event: 'setState', context, payload: { state } });
      this.contexts.set(context, state);
    }
  }

  publishAll() { for (const context of this.contexts.keys()) this.publish(context); }

  refresh() {
    if (!this.contexts.size || this.stopping) return;
    this.statusPending = true;
    this.pump();
  }

  async pump() {
    if (this.busy || this.stopping) return;
    let context;
    while (this.toggles.length && !context) {
      const candidate = this.toggles.shift();
      if (this.contexts.has(candidate)) context = candidate;
    }
    if (!context && !this.statusPending) return;
    this.statusPending = false;
    this.busy = true;
    const generation = this.generation;
    try {
      const status = await this.control.request(context ? 'toggle-mute' : 'status');
      if (!this.stopping && generation === this.generation) { this.status = status; this.publishAll(); }
    } catch {
      if (!this.stopping && generation === this.generation) {
        this.status = null;
        this.publishAll();
        if (context && this.contexts.has(context)) this.send({ event: 'showAlert', context });
      }
    } finally {
      this.busy = false;
      // Both confirmed and ambiguous actions are followed only by a read.
      if (context && this.contexts.size) this.statusPending = true;
      if (!this.stopping && (this.toggles.length || this.statusPending)) queueMicrotask(() => this.pump());
    }
  }

  handle(event) {
    if (event?.action !== ACTION || typeof event.context !== 'string' || !event.context || event.context.length > 256) return;
    const context = event.context;
    if (event.event === 'willAppear') {
      if (!this.contexts.size) this.status = null;
      this.contexts.set(context, undefined);
      this.publish(context, true);
      this.refresh();
    } else if (event.event === 'willDisappear') {
      this.contexts.delete(context);
      this.pressed.delete(context);
      this.toggles = this.toggles.filter(value => value !== context);
      if (!this.contexts.size) this.statusPending = false;
    } else if (event.event === 'keyUp') {
      this.pressed.delete(context);
    } else if (event.event === 'keyDown' && this.contexts.has(context) && !this.pressed.has(context)) {
      this.pressed.add(context);
      if (this.toggles.length >= 8) { this.send({ event: 'showAlert', context }); return; }
      this.toggles.push(context);
      this.pump();
    }
  }

  connect() {
    if (this.stopping) return;
    const socket = this.socket = new WebSocket(`ws://127.0.0.1:${this.port}`);
    socket.addEventListener('open', () => this.send({ event: 'registerPlugin', uuid: this.uuid }));
    socket.addEventListener('message', ({ data }) => {
      try { this.handle(JSON.parse(String(data))); } catch { /* Ignore unrelated malformed deck events. */ }
    });
    socket.addEventListener('error', () => {});
    socket.addEventListener('close', () => {
      ++this.generation;
      this.contexts.clear(); this.pressed.clear(); this.toggles = []; this.statusPending = false; this.status = null;
      if (!this.stopping) this.reconnectTimer = setTimeout(() => this.connect(), this.reconnectMs);
    });
  }

  start() {
    this.connect();
    this.pollTimer = setInterval(() => this.refresh(), this.pollMs);
  }

  stop() {
    this.stopping = true;
    ++this.generation;
    clearInterval(this.pollTimer); clearTimeout(this.reconnectTimer);
    this.socket?.close();
    this.contexts.clear(); this.pressed.clear(); this.toggles = [];
  }
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const args = process.argv.slice(2);
  const argument = name => { const index = args.indexOf(name); return index >= 0 ? args[index + 1] : undefined; };
  const port = Number(argument('-port'));
  const uuid = argument('-pluginUUID');
  if (!Number.isInteger(port) || port < 1 || port > 65535 || !uuid) throw new Error('OpenDeck must launch this plugin.');
  const plugin = new RelayMicPlugin({ port, uuid });
  process.once('SIGTERM', () => { plugin.stop(); process.exit(0); });
  process.once('SIGINT', () => { plugin.stop(); process.exit(0); });
  plugin.start();
}
