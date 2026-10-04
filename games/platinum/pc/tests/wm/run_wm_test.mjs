// The WM model's unit test harness: instantiates test_wm.wasm once per
// station (separate memories, so separate consoles), carries datagrams
// between them through the np_host_net_* imports and checks the outcome.
//
//   node run_wm_test.mjs test_wm.wasm
//
// Scenarios: a clean link; 1 parent + 3 children with 30% loss, 10%
// duplication and up to 4 frames of reordering delay; a child that goes
// silent (the parent must report WM_DISCONNECT_REASON_MP_LIFETIME); and
// networking off (net_self 0: the child scans and finds nobody).
import { readFileSync, writeSync } from 'node:fs';

// The few WASI calls the module makes (stderr, env, exit), so the harness
// runs the same under node and bun.
function miniWasi(env, getMem) {
  const enc = new TextEncoder();
  const vars = Object.entries(env).map(([k, v]) => enc.encode(`${k}=${v}\0`));
  const view = () => new DataView(getMem().buffer);
  return {
    environ_sizes_get: (countPtr, sizePtr) => {
      view().setUint32(countPtr, vars.length, true);
      view().setUint32(sizePtr, vars.reduce((n, v) => n + v.length, 0), true);
      return 0;
    },
    environ_get: (ptrsPtr, bufPtr) => {
      let at = bufPtr;
      vars.forEach((v, i) => {
        view().setUint32(ptrsPtr + 4 * i, at, true);
        new Uint8Array(getMem().buffer, at, v.length).set(v);
        at += v.length;
      });
      return 0;
    },
    clock_time_get: (id, precision, outPtr) => {
      view().setBigUint64(outPtr, 0n, true);
      return 0;
    },
    fd_close: () => 0,
    fd_seek: () => 70, // ESPIPE
    fd_write: (fd, iovs, iovsLen, nwrittenPtr) => {
      let n = 0;
      for (let i = 0; i < iovsLen; i++) {
        const ptr = view().getUint32(iovs + 8 * i, true);
        const len = view().getUint32(iovs + 8 * i + 4, true);
        writeSync(fd === 1 ? 1 : 2, new Uint8Array(getMem().buffer, ptr, len));
        n += len;
      }
      view().setUint32(nwrittenPtr, n, true);
      return 0;
    },
    proc_exit: (code) => {
      throw new Error(`station exited with status ${code}`);
    },
  };
}

const wasmPath = process.argv[2];
if (!wasmPath) {
  console.error('usage: node run_wm_test.mjs test_wm.wasm');
  process.exit(2);
}
const module = await WebAssembly.compile(readFileSync(wasmPath));

const PH = { INIT: 0, READY: 1, SEARCH: 2, LINKED: 3, DONE: 4, LOST: 5 };
const BROADCAST = 0xffffffff;

// Deterministic PRNG so a failure reproduces.
function rng(seed) {
  let s = seed >>> 0;
  return () => {
    s = (s * 1664525 + 1013904223) >>> 0;
    return s / 4294967296;
  };
}

class Air {
  constructor({ drop = 0, dup = 0, maxDelay = 0, seed = 1 } = {}) {
    this.drop = drop;
    this.dup = dup;
    this.maxDelay = maxDelay;
    this.rand = rng(seed);
    this.stations = new Map();
    this.frame = 0;
    this.sent = 0;
    this.dropped = 0;
  }
  deliver(from, to, bytes) {
    const st = this.stations.get(to);
    if (!st || st.offline) return;
    this.sent++;
    if (this.rand() < this.drop) {
      this.dropped++;
      return;
    }
    const copies = this.rand() < this.dup ? 2 : 1;
    for (let i = 0; i < copies; i++) {
      const at = this.frame + Math.floor(this.rand() * (this.maxDelay + 1));
      st.inbox.push({ at, from, bytes });
    }
  }
}

async function station(air, id, role, target) {
  const env = process.env.PC_WM_TRACE ? { PC_WM_TRACE: process.env.PC_WM_TRACE } : {};
  let mem;
  const st = { id, inbox: [], offline: false };
  const np_host = {
    net_self: () => id,
    net_send: (peer, ptr, len) => {
      peer >>>= 0;
      if (st.offline) return 0;
      const bytes = new Uint8Array(mem.buffer, ptr, len).slice();
      if (peer === BROADCAST) {
        for (const other of air.stations.keys()) if (other !== id) air.deliver(id, other, bytes);
      } else {
        air.deliver(id, peer, bytes);
      }
      return 0;
    },
    net_recv: (peerPtr, bufPtr, cap) => {
      const i = st.inbox.findIndex((p) => p.at <= air.frame);
      if (i < 0) return 0;
      const p = st.inbox.splice(i, 1)[0];
      if (p.bytes.length > cap) return -1;
      new Uint8Array(mem.buffer, bufPtr, p.bytes.length).set(p.bytes);
      new DataView(mem.buffer).setUint32(peerPtr, p.from, true);
      return p.bytes.length;
    },
  };
  const instance = await WebAssembly.instantiate(module, { wasi_snapshot_preview1: miniWasi(env, () => mem), np_host });
  mem = instance.exports.memory;
  instance.exports._initialize();
  st.x = instance.exports;
  if (id !== 0) air.stations.set(id, st);
  if (st.x.station_init(role, target) !== 0) throw new Error('station_init failed');
  return st;
}

function run(air, stations, frames, until) {
  for (let f = 0; f < frames; f++) {
    air.frame++;
    for (const s of stations) if (!s.offline) s.x.station_frame();
    if (until && until()) return f + 1;
  }
  return -1;
}

let failures = 0;
function check(name, ok, detail) {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${detail ? '  (' + detail + ')' : ''}`);
  if (!ok) failures++;
}

function errors(stations) {
  return stations.reduce((n, s) => n + s.x.station_errors(), 0);
}

// 1. Clean link: parent + child exchange 300 messages each way, then the
// child disconnects and the parent sees it.
{
  const air = new Air();
  const p = await station(air, 0x000101, 0, 300);
  const c = await station(air, 0x000202, 1, 300);
  const linkedAt = run(air, [p, c], 600, () => p.x.station_phase() === PH.LINKED && c.x.station_phase() === PH.LINKED);
  check('clean: linked', linkedAt > 0, `frame ${linkedAt}, child aid ${c.x.station_aid()}`);
  check('clean: LINK_ACTIVE while linked', p.x.station_link_active() === 1 && c.x.station_link_active() === 1);
  const doneAt = run(air, [p, c], 3000, () => c.x.station_phase() === PH.DONE && p.x.station_phase() === PH.LOST);
  check('clean: 300 msgs each way, in order, then disconnect', doneAt > 0,
    `p rx ${p.x.station_rx()} tx ${p.x.station_tx()}, c rx ${c.x.station_rx()} tx ${c.x.station_tx()}, ` +
    `${doneAt} frames, parent saw reason ${p.x.station_reason()}`);
  check('clean: no error callbacks', errors([p, c]) === 0, `${errors([p, c])}`);
}

// 2. Lossy air, one parent and three children.
{
  const air = new Air({ drop: 0.3, dup: 0.1, maxDelay: 4, seed: 7 });
  const p = await station(air, 0x000101, 0, 1000000);
  const kids = [];
  for (let i = 0; i < 3; i++) kids.push(await station(air, 0x000300 + i, 1, 200));
  const all = [p, ...kids];
  const doneAt = run(air, all, 20000, () => kids.every((k) => k.x.station_phase() === PH.DONE));
  const aids = kids.map((k) => k.x.station_aid()).join(',');
  check('lossy x3: every child sent/received 200 in order and left', doneAt > 0,
    `${doneAt} frames, aids ${aids}, ${air.dropped}/${air.sent} datagrams dropped, ` +
    kids.map((k) => `rx ${k.x.station_rx()}/tx ${k.x.station_tx()}`).join(' '));
  check('lossy x3: distinct AIDs', new Set(kids.map((k) => k.x.station_aid())).size === 3, aids);
  check('lossy x3: parent received all 600', p.x.station_rx() === 600, `${p.x.station_rx()}`);
  check('lossy x3: no error callbacks', errors(all) === 0, `${errors(all)}`);
}

// 3. The child goes silent: the parent drops it after the MP lifetime.
{
  const air = new Air();
  const p = await station(air, 0x000101, 0, 1000000);
  const c = await station(air, 0x000202, 1, 1000000);
  run(air, [p, c], 600, () => c.x.station_phase() === PH.LINKED && c.x.station_rx() > 10);
  c.offline = true;
  const lostAt = run(air, [p], 600, () => p.x.station_phase() === PH.LOST);
  check('lifetime: parent reports MP_LIFETIME', lostAt > 0 && p.x.station_reason() === 0x8001,
    `after ${lostAt} frames, reason ${p.x.station_reason().toString(16)}`);
  check('lifetime: no error callbacks', p.x.station_errors() === 0);
}

// 4. Networking off: the radio works, nobody is in range, no link-active.
{
  const air = new Air();
  const c = await station(air, 0, 1, 10);
  run(air, [c], 300);
  check('offline: scanning, nobody found', c.x.station_phase() === PH.SEARCH && c.x.station_wm_state() === 5,
    `phase ${c.x.station_phase()}, WM state ${c.x.station_wm_state()}`);
  check('offline: LINK_ACTIVE stays 0', c.x.station_link_active() === 0);
  check('offline: no error callbacks', c.x.station_errors() === 0);
}

console.log(failures ? `${failures} FAILED` : 'all passed');
process.exit(failures ? 1 : 0);
