import { useEffect, useRef, useState } from "react";
import type { CoreManifest, GameId } from "../catalog";
import { storage, type SaveSlot } from "../storage";
import { defaultOptions, KEY, type Frame, type Input } from "./abi";
import { GameAudio } from "./audio";
import { FrameProfiler } from "./performance";
export type Settings = {
  layout: "stacked" | "side-by-side";
  effect: boolean;
  volume: number;
  muted: boolean;
  speed: number;
  instantText: boolean;
  fixBugs: boolean;
};
export function useSession(
  settings: Settings,
  onSaved: () => void,
  onError: (message: string) => void,
) {
  const [state, setState] = useState<
    "idle" | "loading" | "running" | "paused" | "error"
  >("idle");
  const profiler = useRef(new FrameProfiler());
  const [saveStatus, setSaveStatus] = useState("No save written yet");
  const [active, setActive] = useState<{ game: GameId; slot: SaveSlot } | null>(
    null,
  );
  const top = useRef<HTMLCanvasElement>(null),
    bottom = useRef<HTMLCanvasElement>(null);
  const worker = useRef<Worker | null>(null),
    audio = useRef<GameAudio | null>(null),
    paused = useRef(false),
    timer = useRef(0);
  const keys = useRef(new Set<string>()),
    touch = useRef<Input>({ keys: 0, touch: false, x: 0, y: 0 });
  const inFlight = useRef(false),
    frameStarted = useRef(0),
    watchdog = useRef(0),
    nextFrameAt = useRef(0);
  const currentSettings = useRef(settings);
  currentSettings.current = settings;
  const callbacks = useRef({ onSaved, onError });
  callbacks.current = { onSaved, onError };
  const saves = useRef(Promise.resolve());
  const latestSave = useRef<ArrayBuffer | undefined>(undefined);
  const stopResolve = useRef<(() => void) | null>(null);
  const draw = (
    canvas: HTMLCanvasElement | null,
    frame: Frame,
    pixels: Uint8ClampedArray,
  ) => {
    if (!canvas) return;
    if (canvas.width !== frame.width) canvas.width = frame.width;
    if (canvas.height !== frame.height) canvas.height = frame.height;
    canvas
      .getContext("2d")
      ?.putImageData(
        new ImageData(
          new Uint8ClampedArray(
            pixels.buffer as ArrayBuffer,
            pixels.byteOffset,
            pixels.byteLength,
          ),
          frame.width,
          frame.height,
        ),
        0,
        0,
      );
  };
  const requestFrame = () => {
    if (paused.current || !worker.current || inFlight.current) return;
    inFlight.current = true;
    frameStarted.current = performance.now();
    if (!nextFrameAt.current) nextFrameAt.current = frameStarted.current;
    let mask = 0;
    for (const k of keys.current) mask |= KEY[k] ?? 0;
    const pad = navigator.getGamepads?.()[0];
    if (pad) {
      const binds = [
        2, 1, 2048, 1024, 512, 256, 0, 0, 4, 8, 0, 0, 64, 128, 32, 16,
      ];
      pad.buttons.forEach((b, i) => {
        if (b.pressed) mask |= binds[i] ?? 0;
      });
      if (pad.axes[0] > 0.4) mask |= 16;
      if (pad.axes[0] < -0.4) mask |= 32;
      if (pad.axes[1] > 0.4) mask |= 128;
      if (pad.axes[1] < -0.4) mask |= 64;
    }
    const opt = defaultOptions(),
      s = currentSettings.current;
    opt[8] = +s.instantText;
    opt[7] = +s.fixBugs;
    clearTimeout(watchdog.current);
    watchdog.current = window.setTimeout(() => {
      paused.current = true;
      inFlight.current = false;
      setState("error");
      audio.current?.pause();
      callbacks.current.onError(
        "The game stopped responding. Close it and try another core.",
      );
      worker.current?.terminate();
      stopResolve.current?.();
      stopResolve.current = null;
    }, 15000);
    worker.current.postMessage({
      type: "frame",
      input: { ...touch.current, keys: mask },
      options: opt,
      speed: profiler.current.stress?.active ? 300 / 59.8261 : s.speed,
    });
  };
  function unlockAudio() {
    if (!audio.current) audio.current = new GameAudio();
    void audio.current
      .resume()
      .catch(() => callbacks.current.onError("Click Resume to enable audio."));
  }
  async function start(game: GameId, slot: SaveSlot, manifest: CoreManifest) {
    if (worker.current) return;
    const core = manifest.games[game];
    if (!core)
      throw Error("This game core has not been prepared for this web build.");
    if (core.verification === "blocked")
      throw Error(core.note || "This core failed its boot check.");
    const rom = await storage.cartridge(game);
    if (!rom) throw Error("Import this cartridge first.");
    if (!audio.current) audio.current = new GameAudio();
    const sound = audio.current;
    void sound.resume();
    sound.volume(settings.muted ? 0 : settings.volume);
    setActive({ game, slot });
    setState("loading");
    latestSave.current = slot.data;
    setSaveStatus(slot.data ? "Loaded from browser storage" : "New save slot");
    paused.current = false;
    const w = new Worker(new URL("./worker.ts", import.meta.url), {
      type: "module",
    });
    worker.current = w;
    const fail = (message: string) => {
      paused.current = true;
      inFlight.current = false;
      clearTimeout(timer.current);
      clearTimeout(watchdog.current);
      setState("error");
      sound.pause();
      callbacks.current.onError(message);
      stopResolve.current?.();
      stopResolve.current = null;
    };
    watchdog.current = window.setTimeout(() => {
      fail(
        "The game did not finish booting. This core may need more porting work.",
      );
      w.terminate();
    }, 45000);
    w.onerror = (e) =>
      fail(e.message || "The game worker stopped unexpectedly.");
    w.onmessage = ({ data }) => {
      if (data.type === "ready") {
        clearTimeout(watchdog.current);
        setState(paused.current ? "paused" : "running");
        if (!paused.current) profiler.current.begin(performance.now());
        requestFrame();
      }
      if (data.type === "frame") {
        inFlight.current = false;
        clearTimeout(watchdog.current);
        const received = performance.now();
        const frame = data.frame as Frame;
        draw(top.current, frame, frame.top);
        draw(bottom.current, frame, frame.bottom);
        profiler.current.rendered(
          data.ticks,
          data.workerMs,
          received - frameStarted.current,
          performance.now() - received,
        );
        if (!paused.current) {
          const current = currentSettings.current;
          if (!current.muted && current.volume > 0)
            sound.play(frame.audio, frame.rate, data.speed);
          const target = profiler.current.stress?.active
            ? 300
            : 59.8261 * current.speed;
          const period = 1000 / target;
          const now = performance.now();
          // Absolute deadlines recover timer jitter instead of adding it every frame.
          nextFrameAt.current = Math.max(
            nextFrameAt.current + period,
            now - period,
          );
          const delay = nextFrameAt.current - now;
          if (delay > 0) timer.current = window.setTimeout(requestFrame, delay);
          else requestFrame();
        }
      }
      if (data.type === "save") {
        latestSave.current = data.data;
        setSaveStatus("Writing save…");
        saves.current = saves.current
          .then(() => storage.save(slot.id, data.data))
          .then(() => {
            setSaveStatus("Saved to this browser");
            callbacks.current.onSaved();
          })
          .catch((e) => {
            setSaveStatus("Save failed — export a backup");
            callbacks.current.onError(String(e));
            paused.current = true;
            setState("paused");
            sound.pause();
          });
      }
      if (data.type === "error") fail(data.message);
      if (data.type === "flushed") {
        stopResolve.current?.();
        stopResolve.current = null;
      }
    };
    const url = new URL(
      `${import.meta.env.BASE_URL}cores/${core.file}`,
      document.baseURI,
    ).href;
    w.postMessage(
      {
        type: "load",
        url,
        sha256: core.sha256,
        rom: rom.data,
        save: slot.data,
      },
      [rom.data],
    );
  }
  function pause() {
    if (!worker.current || state === "loading" || state === "error") return;
    paused.current = !paused.current;
    setState(paused.current ? "paused" : "running");
    clearTimeout(timer.current);
    nextFrameAt.current = 0;
    keys.current.clear();
    touch.current.touch = false;
    if (paused.current) {
      profiler.current.end();
      audio.current?.pause();
      worker.current.postMessage({ type: "flush" });
    } else {
      profiler.current.begin(performance.now());
      void audio.current?.resume();
      requestFrame();
    }
  }
  function benchmark() {
    if (state !== "running" && state !== "paused") return;
    if (paused.current) pause();
    nextFrameAt.current = 0;
    profiler.current.startStress(performance.now());
  }
  async function stop() {
    const w = worker.current;
    if (!w) return;
    paused.current = true;
    profiler.current.end();
    clearTimeout(timer.current);
    audio.current?.pause();
    if (state !== "error" && state !== "loading")
      await new Promise<void>((resolve) => {
        stopResolve.current = resolve;
        w.postMessage({ type: "flush" });
      });
    await saves.current;
    clearTimeout(watchdog.current);
    inFlight.current = false;
    w.terminate();
    worker.current = null;
    audio.current?.close();
    audio.current = null;
    keys.current.clear();
    touch.current.touch = false;
    nextFrameAt.current = 0;
    setState("idle");
    setActive(null);
    callbacks.current.onSaved();
  }
  useEffect(() => {
    if (state !== "running") {
      profiler.current.end();
      return;
    }
    let id = 0;
    const observe = () => {
      profiler.current.refresh(performance.now());
      id = requestAnimationFrame(observe);
    };
    id = requestAnimationFrame(observe);
    return () => cancelAnimationFrame(id);
  }, [state]);
  useEffect(() => {
    audio.current?.volume(settings.muted ? 0 : settings.volume);
  }, [settings.muted, settings.volume]);
  useEffect(() => {
    function down(e: KeyboardEvent) {
      if (
        !worker.current ||
        paused.current ||
        e.ctrlKey ||
        e.metaKey ||
        e.altKey ||
        (e.target as HTMLElement).closest("input,select,textarea,dialog")
      )
        return;
      if (KEY[e.code]) {
        e.preventDefault();
        keys.current.add(e.code);
      }
    }
    function up(e: KeyboardEvent) {
      keys.current.delete(e.code);
    }
    function blur() {
      keys.current.clear();
      touch.current.touch = false;
      if (
        profiler.current.stress?.active &&
        document.visibilityState === "visible"
      )
        return;
      if (worker.current && !paused.current) {
        paused.current = true;
        profiler.current.end();
        clearTimeout(timer.current);
        audio.current?.pause();
        setState((s) => (s === "running" ? "paused" : s));
        worker.current.postMessage({ type: "flush" });
      }
    }
    function visibility() {
      if (document.visibilityState === "hidden") blur();
    }
    document.addEventListener("visibilitychange", visibility);
    window.addEventListener("keydown", down);
    window.addEventListener("keyup", up);
    window.addEventListener("blur", blur);
    return () => {
      document.removeEventListener("visibilitychange", visibility);
      window.removeEventListener("keydown", down);
      window.removeEventListener("keyup", up);
      window.removeEventListener("blur", blur);
      clearTimeout(timer.current);
      clearTimeout(watchdog.current);
      worker.current?.terminate();
      audio.current?.close();
    };
  }, []);
  return {
    state,
    active,
    saveStatus,
    profiler: profiler.current,
    top,
    bottom,
    keys,
    touch,
    latestSave,
    unlockAudio,
    start,
    pause,
    benchmark,
    stop,
  };
}
