import type { GameId } from "../catalog";
import { defaultOptions, OPT } from "./abi";

export type Settings = {
  layout: "stacked";
  effect: boolean;
  volume: number;
  muted: boolean;
  speed: number;
  musicVolume: number;
  effectsVolume: number;
  renderScale: number;
  widescreen: boolean;
  cameraZoom: number;
  cameraTilt: number;
  instantText: boolean;
  fixBugs: boolean;
};

export const initialSettings: Settings = {
  layout: "stacked",
  effect: false,
  volume: 0.65,
  muted: false,
  speed: 1,
  musicVolume: 100,
  effectsVolume: 100,
  renderScale: 1,
  widescreen: false,
  cameraZoom: 256,
  cameraTilt: 0,
  instantText: false,
  fixBugs: false,
};

// Read older preference records without losing their existing values.
export function normalizeSettings(value: unknown): Settings {
  const s =
    value && typeof value === "object"
      ? (value as Record<string, unknown>)
      : {};
  const number = (key: keyof Settings, min: number, max: number) =>
    typeof s[key] === "number" && Number.isFinite(s[key])
      ? Math.max(min, Math.min(max, s[key] as number))
      : (initialSettings[key] as number);
  return {
    layout: "stacked",
    effect: s.effect === true,
    muted: s.muted === true,
    volume: number("volume", 0, 1),
    speed: s.speed === 2 ? 2 : 1,
    musicVolume: Math.round(number("musicVolume", 0, 100)),
    effectsVolume: Math.round(number("effectsVolume", 0, 100)),
    renderScale: Math.round(number("renderScale", 1, 4)),
    widescreen: s.widescreen === true,
    cameraZoom: Math.round(number("cameraZoom", 64, 1024)),
    cameraTilt: Math.round(number("cameraTilt", -720, 720)),
    instantText: s.instantText === true,
    fixBugs: s.fixBugs === true,
  };
}

/** All current guest cores implement these shared options. */
export function gameOptions(s: Settings, _game: GameId, quickSaveSequence = 0) {
  const o = defaultOptions();
  o[OPT.renderScale] = s.renderScale;
  o[OPT.widescreen] = +s.widescreen;
  o[OPT.musicVolume] = Math.floor((s.musicVolume * 256) / 100);
  o[OPT.effectsVolume] = Math.floor((s.effectsVolume * 256) / 100);
  o[OPT.cameraZoom] = s.cameraZoom;
  o[OPT.cameraTilt] = s.cameraTilt;
  o[OPT.quickSaveSequence] = quickSaveSequence;
  o[OPT.instantText] = +s.instantText;
  o[OPT.rules] = +s.fixBugs;
  return o;
}
