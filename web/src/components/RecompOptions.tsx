import type { GameId } from "../catalog";
import { initialSettings, type Settings } from "../runtime/settings";

export function RecompOptions({
  game,
  settings: s,
  onChange,
}: {
  game: GameId;
  settings: Settings;
  onChange: (settings: Settings) => void;
}) {
  const platinum = game === "platinum";
  const set = <K extends keyof Settings>(key: K, value: Settings[K]) =>
    onChange({ ...s, [key]: value });
  return (
    <>
      <h3 className="settings-heading">
        Recomp options /{" "}
        {platinum ? "Platinum" : game === "diamond" ? "Diamond" : "Pearl"}
      </h3>
      <label>
        <span>
          3D resolution
          <small>Higher resolutions use more processing power.</small>
        </span>
        <select
          value={s.renderScale}
          onChange={(e) => set("renderScale", Number(e.target.value))}
        >
          <option value={1}>1× / original</option>
          <option value={2}>2×</option>
          <option value={3}>3×</option>
          <option value={4}>4×</option>
        </select>
      </label>
      <label>
        <span>
          Widescreen 3D<small>Wider field of view; menus stay centred.</small>
        </span>
        <input
          type="checkbox"
          checked={s.widescreen}
          onChange={(e) => set("widescreen", e.target.checked)}
        />
      </label>
      {platinum ? (
        <>
          <label>
            <span>
              Music volume<small>{s.musicVolume}% · includes fanfares</small>
            </span>
            <input
              aria-label="Music volume"
              type="range"
              min={0}
              max={100}
              step={5}
              value={s.musicVolume}
              onChange={(e) => set("musicVolume", Number(e.target.value))}
            />
          </label>
          <label>
            <span>
              Sound effects<small>{s.effectsVolume}% · includes cries</small>
            </span>
            <input
              aria-label="Sound effects"
              type="range"
              min={0}
              max={100}
              step={5}
              value={s.effectsVolume}
              onChange={(e) => set("effectsVolume", Number(e.target.value))}
            />
          </label>
          <label>
            <span>
              Camera distance
              <small>
                {Math.round((s.cameraZoom * 100) / 256)}% · field scenes
              </small>
            </span>
            <input
              aria-label="Camera distance"
              type="range"
              min={64}
              max={1024}
              step={32}
              value={s.cameraZoom}
              onChange={(e) => set("cameraZoom", Number(e.target.value))}
            />
          </label>
          <label>
            <span>
              Camera tilt<small>{s.cameraTilt / 16}° · field scenes</small>
            </span>
            <input
              aria-label="Camera tilt"
              type="range"
              min={-720}
              max={720}
              step={80}
              value={s.cameraTilt}
              onChange={(e) => set("cameraTilt", Number(e.target.value))}
            />
          </label>
          <button
            onClick={() => onChange({ ...s, cameraZoom: 256, cameraTilt: 0 })}
          >
            Reset camera
          </button>
          <label>
            <span>
              Instant text<small>Show dialogue without the wait.</small>
            </span>
            <input
              type="checkbox"
              checked={s.instantText}
              onChange={(e) => set("instantText", e.target.checked)}
            />
          </label>
          <label>
            <span>
              Cartridge bug fixes
              <small>Fix Wonder Guard, Rage, and trainer form stats.</small>
            </span>
            <input
              type="checkbox"
              checked={s.fixBugs}
              onChange={(e) => set("fixBugs", e.target.checked)}
            />
          </label>
        </>
      ) : (
        <p className="settings-note">
          Camera, separate audio controls, instant text, bug fixes and quick
          save are currently supported by Platinum.
        </p>
      )}
      <button
        onClick={() =>
          onChange({
            ...s,
            renderScale: initialSettings.renderScale,
            widescreen: false,
            musicVolume: 100,
            effectsVolume: 100,
            cameraZoom: 256,
            cameraTilt: 0,
            instantText: false,
            fixBugs: false,
          })
        }
      >
        Reset recomp options
      </button>
    </>
  );
}
