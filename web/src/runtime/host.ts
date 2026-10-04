import { ABI, defaultOptions, type Frame, type Input } from "./abi";
type Exports = WebAssembly.Exports & {
  memory: WebAssembly.Memory;
  __stack_pointer: WebAssembly.Global;
  _start: () => void;
  np_fiber_entry: (arg: number) => void;
  malloc: (size: number) => number;
  free: (ptr: number) => void;
  asyncify_start_unwind: (ptr: number) => void;
  asyncify_stop_unwind: () => void;
  asyncify_start_rewind: (ptr: number) => void;
  asyncify_stop_rewind: () => void;
  asyncify_get_state: () => number;
};
type Fiber = {
  id: number;
  arg: number;
  stack: number;
  data: number;
  started: boolean;
};
const STACK_BYTES = 512 * 1024;
/** Browser implementation of np_guest_abi.h. No UI or storage dependencies. */
export class GuestHost {
  exports!: Exports;
  fibers = new Map<number, Fiber>();
  current = 1;
  next = 1;
  serial = 1;
  descriptor = 0;
  audioHead = 0;
  parked = false;
  stopped = false;
  options = defaultOptions();
  input: Input = { keys: 0, touch: false, x: 0, y: 0 };
  constructor(
    readonly rom: Uint8Array,
    public save: Uint8Array | undefined,
    readonly onSave: (data: Uint8Array) => void,
    readonly log: (text: string) => void = console.log,
  ) {}
  bytes(ptr: number, size: number) {
    ptr >>>= 0;
    size >>>= 0;
    if (ptr + size > this.exports.memory.buffer.byteLength)
      throw Error("Guest memory access out of bounds");
    return new Uint8Array(this.exports.memory.buffer, ptr, size);
  }
  view() {
    return new DataView(this.exports.memory.buffer);
  }
  text(ptr: number, size: number) {
    return new TextDecoder().decode(this.bytes(ptr, size));
  }
  u32(ptr: number, value: number) {
    this.view().setUint32(ptr, value, true);
  }
  suspend(to: number, descriptor = 0) {
    if (this.exports.asyncify_get_state() === 2) {
      this.exports.asyncify_stop_rewind();
      return;
    }
    if (!this.fibers.has(to)) throw Error(`Unknown fiber ${to}`);
    if (descriptor) {
      this.validateDescriptor(descriptor);
      this.descriptor = descriptor;
      this.parked = true;
    } else if (to === this.current) return;
    const fiber = this.fibers.get(this.current)!;
    fiber.stack = Number(this.exports.__stack_pointer.value);
    this.next = to;
    this.exports.asyncify_start_unwind(fiber.data);
  }
  newFiber(stack: number, arg: number) {
    if (
      this.fibers.size >= 64 ||
      !stack ||
      stack % 16 ||
      stack > this.exports.memory.buffer.byteLength
    )
      throw Error("Invalid guest fiber");
    const data = this.exports.malloc(STACK_BYTES + 8);
    if (!data) throw Error("Cannot allocate browser fiber stack");
    this.u32(data, data + 8);
    this.u32(data + 4, data + STACK_BYTES + 8);
    const id = this.serial++;
    this.fibers.set(id, { id, stack, arg, data, started: false });
    return id;
  }
  async load(wasm: BufferSource) {
    const host = {
      fiber_self: () => this.current,
      fiber_create: (sp: number, arg: number) => this.newFiber(sp, arg),
      fiber_switch: (to: number) => this.suspend(to),
      fiber_destroy: (id: number) => {
        const f = this.fibers.get(id);
        if (!f || id === this.current) throw Error("Invalid fiber destroy");
        this.exports.free(f.data);
        this.fibers.delete(id);
      },
      vblank: (ptr: number) => this.suspend(this.current, ptr),
      rom_size: () => this.rom.length,
      rom_read: (offset: number, ptr: number, size: number) => {
        offset >>>= 0;
        size >>>= 0;
        if (offset + size > this.rom.length) return -1;
        this.bytes(ptr, size).set(this.rom.subarray(offset, offset + size));
        return 0;
      },
      save_load: (ptr: number, size: number) => {
        if (!this.save) return 0;
        if (size !== this.save.length) return -1;
        this.bytes(ptr, size).set(this.save);
        return 1;
      },
      save_store: (ptr: number, size: number) => {
        this.persist(ptr, size);
        return 0;
      },
      rtc_now: () =>
        BigInt(
          Math.floor((Date.now() - new Date(2000, 0, 1).getTime()) / 1000),
        ),
      log: (ptr: number, size: number) => this.log(this.text(ptr, size)),
      trap: (ptr: number, size: number) => {
        throw Error(this.text(ptr, size));
      },
      net_self: () => 0,
      net_send: () => -1,
      net_recv: () => 0,
      gba_rom_size: () => 0,
      gba_rom_read: () => -1,
      gba_save_load: () => 0,
      gba_save_store: () => -1,
    };
    const zeroPair = (a: number, b: number) => {
      this.u32(a, 0);
      this.u32(b, 0);
      return 0;
    };
    const wasi = {
      args_sizes_get: zeroPair,
      args_get: () => 0,
      environ_sizes_get: zeroPair,
      environ_get: () => 0,
      clock_time_get: (id: number, _precision: bigint, out: number) => {
        this.view().setBigUint64(
          out,
          BigInt(Math.floor((id === 0 ? Date.now() : performance.now()) * 1e6)),
          true,
        );
        return 0;
      },
      random_get: (ptr: number, size: number) => {
        for (let off = 0; off < size; off += 65536)
          crypto.getRandomValues(
            this.bytes(ptr + off, Math.min(65536, size - off)),
          );
        return 0;
      },
      fd_write: (fd: number, iovs: number, count: number, out: number) => {
        if (fd !== 1 && fd !== 2) return 8;
        let n = 0;
        for (let i = 0; i < count; i++) {
          const v = this.view(),
            p = v.getUint32(iovs + i * 8, true),
            len = v.getUint32(iovs + i * 8 + 4, true);
          this.log(this.text(p, len));
          n += len;
        }
        this.u32(out, n);
        return 0;
      },
      fd_read: (fd: number, _i: number, _n: number, out: number) => {
        if (fd !== 0) return 8;
        this.u32(out, 0);
        return 0;
      },
      fd_fdstat_get: (fd: number, out: number) => {
        if (fd > 2) return 8;
        this.bytes(out, 24).fill(0);
        this.bytes(out, 1)[0] = 2;
        this.view().setBigUint64(out + 8, fd === 0 ? 2n : 64n, true);
        return 0;
      },
      fd_fdstat_set_flags: (fd: number, flags: number) =>
        fd > 2 ? 8 : flags ? 58 : 0,
      fd_close: (fd: number) => (fd > 2 ? 8 : 0),
      fd_seek: () => 70,
      fd_tell: () => 70,
      fd_prestat_get: () => 8,
      fd_prestat_dir_name: () => 8,
      fd_filestat_get: () => 8,
      fd_pread: () => 8,
      fd_readdir: () => 8,
      path_open: () => 8,
      path_filestat_get: () => 8,
      path_create_directory: () => 8,
      sched_yield: () => 0,
      proc_exit: (code: number) => {
        throw Error(`Game exited (${code})`);
      },
    };
    const result = await WebAssembly.instantiate(wasm, {
      np_host: host,
      wasi_snapshot_preview1: wasi,
    });
    this.exports = result.instance.exports as Exports;
    if (!this.exports.asyncify_start_unwind || !this.exports.__stack_pointer)
      throw Error("This core needs the browser preparation step");
    this.newFiber(Number(this.exports.__stack_pointer.value), 0);
  }
  validateDescriptor(ptr: number) {
    this.bytes(ptr, ABI.descriptorBytes);
    const v = this.view();
    if (
      ptr % 4 ||
      v.getUint32(ptr, true) !== ABI.magic ||
      v.getUint32(ptr + 4, true) !== ABI.version
    )
      throw Error("Incompatible game core ABI");
    if (this.descriptor && this.descriptor !== ptr)
      throw Error("Guest descriptor moved");
    const w = v.getUint32(ptr + 16, true),
      h = v.getUint32(ptr + 20, true),
      stride = v.getUint32(ptr + 24, true);
    if (
      w < 256 ||
      w > 2048 ||
      h < 192 ||
      h > 1536 ||
      stride < w ||
      stride > 2048
    )
      throw Error("Invalid frame geometry");
    for (const offset of [8, 12])
      this.bytes(v.getUint32(ptr + offset, true), stride * h * 4);
  }
  persist(ptr: number, size: number) {
    this.save = this.bytes(ptr, size).slice();
    this.onSave(this.save);
  }
  flush() {
    if (!this.descriptor) return;
    const d = this.descriptor,
      v = this.view();
    if (v.getUint32(d + 60, true)) {
      this.persist(v.getUint32(d + 56, true), v.getUint32(d + 52, true));
      this.u32(d + 60, 0);
    }
  }
  runFrame(count = 1): Frame {
    if (!Number.isInteger(count) || count < 1 || count > 4)
      throw Error("Invalid frame batch");
    // Run every game tick and save callback, but only copy the final picture.
    // Audio stays in the guest ring until the whole batch is captured.
    for (let i = 0; i < count; i++) this.advanceFrame();
    return this.frame();
  }
  private advanceFrame() {
    if (this.stopped) throw Error("Guest stopped");
    if (this.descriptor) {
      const d = this.descriptor;
      const values = [
        this.input.keys,
        +this.input.touch,
        this.input.x,
        this.input.y,
        0,
        0,
      ];
      values.forEach((x, i) => this.u32(d + ABI.input + i * 4, x));
      this.options.forEach((x, i) => this.u32(d + ABI.options + i * 4, x));
    }
    this.parked = false;
    let switches = 0;
    while (!this.parked) {
      if (++switches > 100000)
        throw Error("Core failed to reach a frame boundary");
      const f = this.fibers.get(this.next)!;
      this.current = f.id;
      this.exports.__stack_pointer.value = f.stack;
      if (f.started) this.exports.asyncify_start_rewind(f.data);
      f.started = true;
      if (f.id === 1) this.exports._start();
      else this.exports.np_fiber_entry(f.arg);
      if (this.exports.asyncify_get_state() !== 1)
        throw Error("Guest fiber returned unexpectedly");
      this.exports.asyncify_stop_unwind();
    }
    this.flush();
  }
  frame(): Frame {
    const v = this.view(),
      d = this.descriptor,
      get = (off: number) => v.getUint32(d + off, true);
    const width = get(16),
      height = get(20),
      stride = get(24);
    const pixels = (ptr: number) => {
      const out = new Uint8ClampedArray(width * height * 4);
      for (let y = 0; y < height; y++)
        for (let x = 0; x < width; x++) {
          const c = v.getUint32(ptr + (y * stride + x) * 4, true),
            i = (y * width + x) * 4;
          out[i] = (c >>> 16) & 255;
          out[i + 1] = (c >>> 8) & 255;
          out[i + 2] = c & 255;
          out[i + 3] = 255;
        }
      return out;
    };
    const cap = get(40),
      head = get(44),
      rate = get(48),
      ptr = get(36);
    if (cap && (cap & (cap - 1) || cap > 1 << 24))
      throw Error("Invalid audio ring");
    this.bytes(ptr, cap * 4);
    const count = Math.min((head - this.audioHead) >>> 0, cap);
    const audio = new Float32Array(count * 2);
    for (let i = 0; i < count; i++) {
      const p = ptr + (((head - count + i) >>> 0) & (cap - 1)) * 4;
      audio[i * 2] = v.getInt16(p, true) / 32768;
      audio[i * 2 + 1] = v.getInt16(p + 2, true) / 32768;
    }
    this.audioHead = head;
    return {
      width,
      height,
      top: pixels(get(8)),
      bottom: pixels(get(12)),
      audio,
      rate,
      number: get(28),
      status: {
        linkActive: get(ABI.status) !== 0,
        fieldReady: get(ABI.status + 4) !== 0,
        quickSaveSequence: get(ABI.status + 8),
        quickSaveResult: get(ABI.status + 12),
        mapId: get(ABI.status + 16),
      },
    };
  }
}
