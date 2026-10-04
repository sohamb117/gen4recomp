import {
  useEffect,
  useRef,
  useState,
  type CSSProperties,
  type PointerEvent,
} from "react";
import {
  Upload,
  Play,
  Pause,
  Download,
  Maximize2,
  Settings2,
  Save,
  Volume2,
  VolumeX,
  FastForward,
  HardDrive,
  Keyboard,
  X,
  Plus,
  ChevronRight,
} from "lucide-react";
import { Circuit } from "./components/Circuit";
import { Panel } from "./components/Panel";
import { Modal } from "./components/Modal";
import { PerformanceStats } from "./components/PerformanceStats";
import { TouchControls } from "./components/TouchControls";
import { games, identifyRom, type CoreManifest, type GameId } from "./catalog";
import {
  fetchCartridge,
  loadCartridges,
  type HostedCartridges,
} from "./cartridges";
import { download, saveData, storage, type SaveSlot } from "./storage";
import { useSession } from "./runtime/useSession";
import {
  initialSettings,
  normalizeSettings,
  type Settings,
} from "./runtime/settings";
import { mapTouch } from "./runtime/touch";
import { RecompOptions } from "./components/RecompOptions";
type Popup = "saves" | "settings" | "controls" | "performance" | null;
function readSettings(): Settings {
  try {
    return normalizeSettings(
      JSON.parse(localStorage.getItem("nativeplat-settings") || "{}"),
    );
  } catch {
    return initialSettings;
  }
}
export function App() {
  const [hosted, setHosted] = useState<HostedCartridges>({});
  const [popup, setPopup] = useState<Popup>(null),
    [selected, setSelected] = useState<GameId>("platinum");
  const [roms, setRoms] = useState<GameId[]>([]),
    [slots, setSlots] = useState<SaveSlot[]>([]),
    [manifest, setManifest] = useState<CoreManifest | null>(null);
  const [notice, setNotice] = useState(""),
    [busy, setBusy] = useState(false),
    [settings, setSettings] = useState(readSettings),
    [slotName, setSlotName] = useState("My journey");
  const romInput = useRef<HTMLInputElement>(null),
    saveInput = useRef<HTMLInputElement>(null),
    display = useRef<HTMLDivElement>(null);
  const refresh = async () => {
    const [r, s] = await Promise.all([storage.cartridgeIds(), storage.slots()]);
    setRoms(r);
    setSlots(s.sort((a, b) => b.updated - a.updated));
  };
  const session = useSession(
    settings,
    () => void refresh().catch((e) => setNotice(String(e))),
    setNotice,
  );
  const game = games.find((g) => g.id === selected)!,
    gameSlots = slots.filter((s) => s.game === selected);
  useEffect(() => {
    void refresh().catch((e) => setNotice(`Browser storage unavailable: ${e}`));
    void loadCartridges(import.meta.env.BASE_URL)
      .then(setHosted)
      .catch((e) => setNotice(String(e)));
    fetch(`${import.meta.env.BASE_URL}cores/manifest.json`)
      .then(async (r) => {
        if (!r.ok) throw Error();
        const m = await r.json();
        if (m.version !== 1 || m.abi !== 2 || m.runtime !== "asyncify")
          throw Error();
        setManifest(m);
      })
      .catch(() =>
        setNotice(
          "Game cores are not included in this build yet. You can still organize your cartridges and saves.",
        ),
      );
  }, []);
  useEffect(() => {
    try {
      localStorage.setItem("nativeplat-settings", JSON.stringify(settings));
    } catch {
      /* Storage failure is surfaced when saving game data. */
    }
  }, [settings]);
  async function action(fn: () => Promise<unknown>) {
    if (busy) return;
    setBusy(true);
    try {
      await fn();
    } catch (e) {
      setNotice(e instanceof Error ? e.message : String(e));
    } finally {
      setBusy(false);
    }
  }
  async function importRom(file: File) {
    await action(async () => {
      const data = await file.arrayBuffer(),
        g = await identifyRom(data);
      await storage.importRom({
        game: g.id,
        name: file.name,
        data,
        imported: Date.now(),
      });
      setSelected(g.id);
      await refresh();
      setNotice(`${g.name} imported. Your cartridge stays on this device.`);
    });
  }
  async function newSlot(data?: ArrayBuffer) {
    const slot: SaveSlot = {
      id: crypto.randomUUID(),
      game: selected,
      name: slotName.trim().slice(0, 32) || "My journey",
      updated: Date.now(),
      data,
    };
    await storage.putSlot(slot);
    await refresh();
    return slot;
  }
  async function launch(slot?: SaveSlot) {
    session.unlockAudio();
    await action(async () => {
      if (!manifest)
        throw Error("Browser cores are unavailable in this build.");
      if (!manifest.games[selected])
        throw Error(`${game.name} is not built for this release yet.`);
      if (!(await storage.cartridge(selected))) {
        const entry = hosted[selected];
        if (!entry) throw Error("Import a cartridge to play this game.");
        let percent = -1;
        const data = await fetchCartridge(
          import.meta.env.BASE_URL,
          selected,
          entry,
          (fraction) => {
            const next = Math.floor(fraction * 100);
            if (next !== percent) {
              percent = next;
              setNotice(
                percent === 100
                  ? `Preparing ${game.name}…`
                  : `Downloading ${game.name}… ${percent}%`,
              );
            }
          },
        );
        await storage.importRom({
          game: selected,
          name: entry.file,
          data,
          imported: Date.now(),
        });
        await refresh();
        setNotice("");
      }
      const target = slot ?? gameSlots[0] ?? (await newSlot());
      await session.start(selected, target, manifest);
      setPopup(null);
    });
  }
  function stylus(e: PointerEvent<HTMLCanvasElement>) {
    const r = e.currentTarget.getBoundingClientRect();
    const point = mapTouch(
      e.clientX - r.left,
      e.clientY - r.top,
      r.width,
      r.height,
      e.currentTarget.width,
      e.currentTarget.height,
    );
    session.touch.current = {
      ...session.touch.current,
      touch:
        point.inside && e.type !== "pointerup" && e.type !== "pointercancel",
      x: point.x,
      y: point.y,
    };
    if (e.type === "pointerdown")
      e.currentTarget.setPointerCapture(e.pointerId);
  }
  function touchKey(code: string, down: boolean) {
    if (down) session.keys.current.add(code);
    else session.keys.current.delete(code);
  }
  function openPopup(name: Exclude<Popup, null>) {
    session.keys.current.clear();
    session.touch.current.touch = false;
    setPopup(name);
  }
  const running = !!session.active,
    ready =
      (roms.includes(selected) || !!hosted[selected]) &&
      !!manifest?.games[selected] &&
      manifest.games[selected]?.verification !== "blocked";
  return (
    <>
      <Circuit />
      <div
        className="workspace"
        onDragOver={(e) => {
          e.preventDefault();
        }}
        onDrop={(e) => {
          e.preventDefault();
          const f = e.dataTransfer.files[0];
          if (f && !running) void importRom(f);
        }}
      >
        {notice && (
          <div className="notice" role="status">
            <span>{notice}</span>
            <button aria-label="Dismiss message" onClick={() => setNotice("")}>
              <X size={14} />
            </button>
          </div>
        )}
        <main className="main-grid">
          <aside className="library-column">
            <div className="cartridge-rail" aria-label="Cartridges">
              <div className="cartridges">
                {games.map((g, i) => (
                  <button
                    key={g.id}
                    disabled={running || busy}
                    className={`cartridge ${selected === g.id ? "selected" : ""}`}
                    style={{ "--game-color": g.color } as CSSProperties}
                    onClick={() => setSelected(g.id)}
                    aria-pressed={selected === g.id}
                  >
                    <span className="cart-meta">
                      0{i + 1}
                      <span>{g.year}</span>
                    </span>
                    <div className={`gem gem-${g.id}`} aria-hidden="true">
                      <span>{g.symbol}</span>
                      <i />
                      <b />
                    </div>
                    <div className="cart-label">
                      <small>POKÉMON</small>
                      <h2>{g.name}</h2>
                      <span>{g.code}</span>
                    </div>
                    <div className="cart-status">
                      <i
                        className={
                          roms.includes(g.id) || hosted[g.id] ? "present" : ""
                        }
                      />
                      {roms.includes(g.id)
                        ? "CARTRIDGE CONNECTED"
                        : hosted[g.id]
                          ? "AVAILABLE ONLINE"
                          : "AWAITING CARTRIDGE"}
                      <ChevronRight size={13} />
                    </div>
                  </button>
                ))}
              </div>
              <div className="library-actions">
                <button
                  className="primary-button"
                  disabled={!ready || busy || running}
                  onClick={() => void launch()}
                >
                  <Play size={14} /> {gameSlots[0]?.data ? "Continue" : "Play"}{" "}
                  {game.name}
                </button>
                <button
                  className="import-button"
                  disabled={busy || running}
                  onClick={() => romInput.current?.click()}
                >
                  <Upload size={14} /> Import cartridge
                </button>
                {manifest?.games[selected]?.verification === "blocked" && (
                  <p className="core-note">{manifest.games[selected]?.note}</p>
                )}
              </div>
            </div>
          </aside>
          <div className="game-column">
            <Panel
              title="GAME VIEW"
              className="player-panel"
              aside={
                <span className="live-label">
                  {running ? session.state.toUpperCase() : "STANDBY"}
                </span>
              }
            >
              <div
                ref={display}
                className={`display stacked ${settings.effect ? "lcd" : ""}`}
              >
                {running ? (
                  <>
                    <div className="screen">
                      <canvas ref={session.top} aria-label="Top game screen" />
                    </div>
                    <div className="screen touch-screen">
                      <canvas
                        ref={session.bottom}
                        aria-label="Touch game screen"
                        onPointerDown={stylus}
                        onPointerMove={(e) => {
                          if (e.buttons) stylus(e);
                        }}
                        onPointerUp={stylus}
                        onPointerCancel={stylus}
                      />
                    </div>
                    {session.state !== "running" && (
                      <div className="screen-message">
                        <span>
                          {session.state === "loading"
                            ? "CONNECTING TO SINNOH…"
                            : session.state === "error"
                              ? "SIGNAL INTERRUPTED"
                              : "PAUSED"}
                        </span>
                        {session.state === "paused" && (
                          <button onClick={session.pause}>
                            <Play size={14} /> Resume
                          </button>
                        )}
                      </div>
                    )}
                  </>
                ) : (
                  <>
                    <div className="standby-screen">
                      <div className="crosshair" />
                      <span>NO CARTRIDGE SIGNAL</span>
                      <small>Select a cartridge to begin.</small>
                      <div className="signal-line" />
                    </div>
                    <div className="standby-screen secondary-screen">
                      <span className="standby-symbol">✧</span>
                      <span>TOUCH / 02</span>
                    </div>
                  </>
                )}
              </div>
              <div className="player-toolbar">
                <div>
                  {running ? (
                    <>
                      <button
                        onClick={session.pause}
                        disabled={
                          session.state === "loading" ||
                          session.state === "error"
                        }
                      >
                        {session.state === "paused" ? (
                          <Play size={14} />
                        ) : (
                          <Pause size={14} />
                        )}{" "}
                        {session.state === "paused" ? "Resume" : "Pause"}
                      </button>
                      <button
                        onClick={() => void action(session.stop)}
                        disabled={busy}
                      >
                        Close game
                      </button>
                    </>
                  ) : (
                    <span className="muted">256 × 192</span>
                  )}
                </div>
                <div>
                  <button
                    title={settings.muted ? "Unmute" : "Mute"}
                    aria-label={settings.muted ? "Unmute" : "Mute"}
                    aria-pressed={settings.muted}
                    onClick={() =>
                      setSettings({ ...settings, muted: !settings.muted })
                    }
                  >
                    {settings.muted ? (
                      <VolumeX size={16} />
                    ) : (
                      <Volume2 size={16} />
                    )}
                  </button>
                  <button
                    title="Fast-forward 2×"
                    aria-label="Fast-forward 2×"
                    aria-pressed={settings.speed === 2}
                    onClick={() =>
                      setSettings({
                        ...settings,
                        speed: settings.speed === 2 ? 1 : 2,
                      })
                    }
                  >
                    <FastForward size={16} /> 2×
                  </button>
                  {session.active?.game === "platinum" && (
                    <button
                      title="Quick save (F1)"
                      onClick={session.quickSave}
                      disabled={session.state !== "running" || session.saving}
                    >
                      <Save size={16} />{" "}
                      {session.saving ? "Saving…" : "Quick save"}
                    </button>
                  )}
                  <button
                    title="Save manager"
                    aria-label="Save manager"
                    onClick={() => openPopup("saves")}
                  >
                    <HardDrive size={16} />
                  </button>
                  <button
                    title="Preferences"
                    aria-label="Preferences"
                    onClick={() => openPopup("settings")}
                  >
                    <Settings2 size={16} />
                  </button>
                  <button
                    title="Controls"
                    aria-label="Controls"
                    onClick={() => openPopup("controls")}
                  >
                    <Keyboard size={16} />
                  </button>
                  <button
                    title="Fullscreen"
                    aria-label="Fullscreen"
                    onClick={() =>
                      void display.current
                        ?.requestFullscreen()
                        .catch(() =>
                          setNotice(
                            "Fullscreen is unavailable in this browser.",
                          ),
                        )
                    }
                  >
                    <Maximize2 size={14} />
                  </button>
                </div>
              </div>
              <TouchControls
                onKey={touchKey}
                disabled={!running || session.state !== "running"}
              />
            </Panel>
          </div>
        </main>
        <input
          ref={romInput}
          type="file"
          accept=".nds"
          hidden
          onChange={(e) => {
            const f = e.target.files?.[0];
            e.target.value = "";
            if (f) void importRom(f);
          }}
        />
        <input
          ref={saveInput}
          type="file"
          accept=".sav,.dsv"
          hidden
          onChange={(e) => {
            const f = e.target.files?.[0];
            e.target.value = "";
            if (f)
              void action(async () => {
                await newSlot(saveData(await f.arrayBuffer()));
                setNotice("Save imported into a new slot.");
              });
          }}
        />
        {popup === "saves" && (
          <Modal title="Save manager" onClose={() => setPopup(null)}>
            <div className="archive-head">
              <div className="game-tabs">
                {games.map((g) => (
                  <button
                    className={selected === g.id ? "active" : ""}
                    disabled={running}
                    onClick={() => setSelected(g.id)}
                    key={g.id}
                  >
                    {g.name}
                  </button>
                ))}
              </div>
            </div>
            <div className="slot-create">
              <input
                aria-label="New save slot name"
                maxLength={32}
                value={slotName}
                onChange={(e) => setSlotName(e.target.value)}
              />
              <button
                disabled={busy || running}
                onClick={() => void action(() => newSlot())}
              >
                <Plus size={14} /> New slot
              </button>
              <button
                disabled={busy || running}
                onClick={() => saveInput.current?.click()}
              >
                <Upload size={14} /> Import .sav
              </button>
            </div>
            <div className="slot-list">
              {gameSlots.length === 0 ? (
                <div className="empty-state">
                  <HardDrive size={32} />
                  <h3>No journeys yet.</h3>
                  <p>Create a slot, or bring a save from your cartridge.</p>
                </div>
              ) : (
                gameSlots.map((s) => (
                  <article className="slot" key={s.id}>
                    <HardDrive size={19} />
                    <div>
                      <h3>{s.name}</h3>
                      <p>
                        {s.data
                          ? "512 KiB · " + new Date(s.updated).toLocaleString()
                          : "New journey · no game save yet"}
                      </p>
                    </div>
                    <button
                      disabled={!s.data}
                      title="Export save"
                      aria-label={`Export ${s.name}`}
                      onClick={() =>
                        s.data && download(s.data, `${s.name}.sav`)
                      }
                    >
                      <Download size={15} />
                    </button>
                    {s.backup && (
                      <button
                        onClick={() =>
                          download(s.backup!, `${s.name}.previous.sav`)
                        }
                      >
                        Previous
                      </button>
                    )}
                    <button
                      disabled={!ready || busy || running}
                      onClick={() => void launch(s)}
                    >
                      <Play size={14} /> Play
                    </button>
                  </article>
                ))
              )}
            </div>
          </Modal>
        )}
        {popup === "settings" && (
          <Modal title="Preferences" onClose={() => setPopup(null)}>
            <div className="settings-list">
              <label>
                <span>
                  LCD texture
                  <small>A subtle grid over the game screens.</small>
                </span>
                <input
                  type="checkbox"
                  checked={settings.effect}
                  onChange={(e) =>
                    setSettings({ ...settings, effect: e.target.checked })
                  }
                />
              </label>
              <label>
                <span>
                  Audio volume
                  <small>{Math.round(settings.volume * 100)}%</small>
                </span>
                <input
                  aria-label="Audio volume"
                  type="range"
                  min="0"
                  max="1"
                  step=".05"
                  value={settings.volume}
                  onChange={(e) =>
                    setSettings({
                      ...settings,
                      volume: Number(e.target.value),
                    })
                  }
                />
              </label>
              <label>
                <span>
                  Playback speed
                  <small>Faster travel through familiar places.</small>
                </span>
                <select
                  value={settings.speed}
                  onChange={(e) =>
                    setSettings({
                      ...settings,
                      speed: Number(e.target.value),
                    })
                  }
                >
                  <option value="1">1× / original</option>
                  <option value="2">2×</option>
                </select>
              </label>
              <RecompOptions
                game={session.active?.game ?? selected}
                settings={settings}
                onChange={setSettings}
              />
              <button onClick={() => setPopup("performance")}>
                Performance / FPS
              </button>
            </div>
          </Modal>
        )}
        {popup === "performance" && (
          <Modal title="Performance" onClose={() => setPopup(null)}>
            <PerformanceStats
              profiler={session.profiler}
              state={session.state}
              onPause={session.pause}
              onBenchmark={session.benchmark}
            />
          </Modal>
        )}
        {popup === "controls" && (
          <Modal title="Controls" onClose={() => setPopup(null)}>
            <div className="controls-list">
              {[
                ["Move", "↑ ↓ ← → / W A S D"],
                ["A / B", "Z / X"],
                ["X / Y", "C / V"],
                ["L / R", "Q / E"],
                ["Start / Select", "Esc / Tab"],
                ["Touch screen", "Click or touch the lower screen"],
                ["Quick save / Platinum", "F1"],
              ].map(([a, b]) => (
                <div key={a}>
                  <span>{a}</span>
                  <kbd>{b}</kbd>
                </div>
              ))}
              <p>
                Standard gamepads are supported. The game pauses when you leave
                this window.
              </p>
              <button onClick={() => setPopup(null)}>Close</button>
            </div>
          </Modal>
        )}
      </div>
    </>
  );
}
