import { useEffect, useRef, useState } from "react";
import { generation, isGen4, type CoreManifest, type GameId } from "../catalog";
import { storage, saveData, type SaveSlot } from "../storage";
import { readEngineSave } from "../../shared/save-format.mjs";
import type { Settings } from "./settings";
import type { Input } from "./abi";
import { FrameProfiler } from "./performance";
const KEY: Record<string, string> = {
  ArrowUp: "up",
  ArrowDown: "down",
  ArrowLeft: "left",
  ArrowRight: "right",
  KeyW: "up",
  KeyS: "down",
  KeyA: "left",
  KeyD: "right",
  KeyZ: "z",
  KeyX: "x",
  KeyQ: "q",
  KeyE: "e",
  Escape: "escape",
  Tab: "tab",
  Enter: "return",
  Space: "space",
};
export function useGen123Session(
  settings: Settings,
  onSaved: (slot?: SaveSlot) => void,
  onError: (message: string) => void,
) {
  const [state, setState] = useState<
    "idle" | "loading" | "running" | "paused" | "error"
  >("idle");
  const [active, setActive] = useState<{ game: GameId; slot: SaveSlot } | null>(
    null,
  );
  const [saving, setSaving] = useState(false);
  const [progress, setProgress] = useState("Loading engine…");
  const iframe = useRef<HTMLIFrameElement>(null);
  const keys = useRef(new Set<string>()),
    touch = useRef<Input>({ keys: 0, touch: false, x: 0, y: 0 });
  const top = useRef<HTMLCanvasElement>(null),
    bottom = useRef<HTMLCanvasElement>(null);
  const profiler = useRef(new FrameProfiler()),
    latestSave = useRef<ArrayBuffer | undefined>(undefined);
  const callbacks = useRef({ onSaved, onError });
  callbacks.current = { onSaved, onError };
  const currentSettings = useRef(settings);
  currentSettings.current = settings;
  const status = useRef(state);
  status.current = state;
  const current = useRef<{
    game: GameId;
    slot: SaveSlot;
    rom: ArrayBuffer;
    sha1: string;
    save?: string;
    options?: string;
    channel: string;
  } | null>(null);
  const [channel, setChannel] = useState("");
  const saves = useRef(Promise.resolve());
  const sequence = useRef(0),
    ack = useRef<((ok: boolean) => void) | null>(null);
  const timeout = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);
  const emit = (data: object) =>
    iframe.current?.contentWindow?.postMessage(
      { ...data, channel: current.current?.channel },
      location.origin,
    );
  function quickSave() {
    if (status.current !== "running" || ack.current) return;
    setSaving(true);
    emit({ type: "save", id: ++sequence.current });
    clearTimeout(timeout.current);
    timeout.current = setTimeout(() => {
      setSaving(false);
      callbacks.current.onError(
        "Save request timed out. Keep the game open and retry.",
      );
    }, 15000);
  }
  async function stop() {
    if (!current.current) return;
    if (status.current === "running" || status.current === "paused") {
      status.current = "paused";
      setState("paused");
      keys.current.clear();
      const ok = await new Promise<boolean>((resolve) => {
        ack.current = resolve;
        emit({ type: "flush", id: ++sequence.current });
        timeout.current = setTimeout(() => {
          ack.current = null;
          resolve(false);
        }, 15000);
      });
      if (!ok)
        throw Error(
          "Could not flush the game save. Keep this window open and retry.",
        );
    }
    await saves.current;
    current.current = null;
    keys.current.clear();
    setActive(null);
    setState("idle");
  }
  async function start(
    game: GameId,
    slot: SaveSlot,
    _manifest?: CoreManifest | null,
  ) {
    if (isGen4(game))
      throw Error("The Gen1Recomp adapter cannot run a DS cartridge.");
    await stop();
    const rom = await storage.cartridge(game);
    if (!rom) throw Error("Import a cartridge to play this game.");
    const sha1 = Array.from(
      new Uint8Array(await crypto.subtle.digest("SHA-1", rom.data)),
      (x) => x.toString(16).padStart(2, "0"),
    ).join("");
    const decoded = slot.data
      ? readEngineSave(new Uint8Array(slot.data), game)
      : undefined;
    const next = crypto.randomUUID();
    current.current = {
      game,
      slot,
      rom: rom.data,
      sha1,
      save: decoded?.save,
      options: decoded?.options,
      channel: next,
    };
    saves.current = Promise.resolve();
    latestSave.current = slot.data;
    setChannel(next);
    setActive({ game, slot });
    setState("loading");
    setProgress("Loading engine…");
  }
  function pause() {
    keys.current.clear();
    setState((s) =>
      s === "running" ? "paused" : s === "paused" ? "running" : s,
    );
  }
  useEffect(() => {
    function message(e: MessageEvent) {
      const session = current.current;
      if (
        !session ||
        e.origin !== location.origin ||
        e.source !== iframe.current?.contentWindow ||
        e.data?.channel !== session.channel
      )
        return;
      const m = e.data;
      if (m.type === "loaded")
        emit({
          type: "start",
          game: session.game,
          rom: session.rom,
          sha1: session.sha1,
          save: session.save,
          options: session.options,
        });
      else if (m.type === "ready") {
        setState("running");
        setProgress("");
      } else if (m.type === "progress") setProgress(String(m.message));
      else if (m.type === "error") {
        setState("error");
        callbacks.current.onError(String(m.message));
      } else if (m.type === "exit")
        void stop().catch((e) => callbacks.current.onError(String(e)));
      else if (m.type === "save") {
        saves.current = saves.current.then(async () => {
          const data = saveData(m.data, session.game);
          await storage.save(session.slot.id, data);
          latestSave.current = data;
          callbacks.current.onSaved({
            ...session.slot,
            data,
            updated: Date.now(),
          });
        });
        void saves.current.catch((e) => {
          setState("error");
          callbacks.current.onError(`Save failed: ${e}. Keep the game open.`);
        });
      } else if (m.type === "ack") {
        clearTimeout(timeout.current);
        setSaving(false);
        if (m.message) callbacks.current.onError(m.message);
        void saves.current.then(
          () => {
            ack.current?.(true);
            ack.current = null;
          },
          () => {
            ack.current?.(false);
            ack.current = null;
          },
        );
      }
    }
    function down(e: KeyboardEvent) {
      if (
        status.current !== "running" ||
        e.ctrlKey ||
        e.metaKey ||
        e.altKey ||
        (e.target as HTMLElement).closest("input,textarea,select,dialog")
      )
        return;
      if (KEY[e.code]) {
        e.preventDefault();
        keys.current.add(e.code);
      }
      if (e.code === "F1") {
        e.preventDefault();
        if (!e.repeat) quickSave();
      }
    }
    function up(e: KeyboardEvent) {
      keys.current.delete(e.code);
    }
    function blur() {
      keys.current.clear();
      setState((s) => (s === "running" ? "paused" : s));
    }
    function visibility() {
      if (document.hidden) blur();
    }
    window.addEventListener("message", message);
    window.addEventListener("keydown", down);
    window.addEventListener("keyup", up);
    window.addEventListener("blur", blur);
    document.addEventListener("visibilitychange", visibility);
    return () => {
      window.removeEventListener("message", message);
      window.removeEventListener("keydown", down);
      window.removeEventListener("keyup", up);
      window.removeEventListener("blur", blur);
      document.removeEventListener("visibilitychange", visibility);
      clearTimeout(timeout.current);
    };
  }, []);
  useEffect(() => {
    if (!active) return;
    let id = 0,
      last = "";
    function tick() {
      const s = currentSettings.current;
      const pressed = new Set(
        [...keys.current].map((k) => KEY[k]).filter(Boolean),
      );
      const pad = navigator.getGamepads?.()[0];
      if (pad) {
        const binds = [
          "x",
          "z",
          "",
          "",
          "q",
          "e",
          "",
          "",
          "tab",
          "escape",
          "",
          "",
          "up",
          "down",
          "left",
          "right",
        ];
        pad.buttons.forEach((b, i) => {
          if (b.pressed && binds[i]) pressed.add(binds[i]);
        });
        if (pad.axes[0] > 0.4) pressed.add("right");
        if (pad.axes[0] < -0.4) pressed.add("left");
        if (pad.axes[1] > 0.4) pressed.add("down");
        if (pad.axes[1] < -0.4) pressed.add("up");
      }
      const command = {
        keys: status.current === "running" ? [...pressed] : [],
        paused: status.current === "paused",
        volume: s.muted ? 0 : s.volume,
        speed: s.speed,
      };
      const next = JSON.stringify(command);
      if (next !== last) {
        last = next;
        emit({ type: "command", command });
      }
      id = requestAnimationFrame(tick);
    }
    id = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(id);
  }, [active, state]);
  return {
    state,
    active,
    saving,
    progress,
    iframe,
    channel,
    keys,
    touch,
    top,
    bottom,
    latestSave,
    saveStatus: "",
    profiler: profiler.current,
    quickSave,
    start,
    pause,
    stop,
    unlockAudio: () => emit({ type: "audio" }),
    benchmark: () => {},
    aspect: active && generation(active.game) === 3 ? "3 / 2" : "10 / 9",
  };
}
