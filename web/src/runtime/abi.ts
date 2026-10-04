export const ABI = {
  version: 2,
  magic: 0x4e504652,
  descriptorBytes: 312,
  status: 64,
  input: 128,
  options: 152,
} as const;
export const OPT = {
  musicVolume: 0,
  effectsVolume: 1,
  renderScale: 2,
  widescreen: 3,
  cameraZoom: 4,
  cameraTilt: 5,
  quickSaveSequence: 6,
  rules: 7,
  instantText: 8,
} as const;
export type GuestStatus = {
  linkActive: boolean;
  fieldReady: boolean;
  quickSaveSequence: number;
  quickSaveResult: number;
  mapId: number;
};
export type Input = { keys: number; touch: boolean; x: number; y: number };
export type Frame = {
  width: number;
  height: number;
  top: Uint8ClampedArray;
  bottom: Uint8ClampedArray;
  audio: Float32Array;
  rate: number;
  number: number;
  status: GuestStatus;
};
export const defaultOptions = () => {
  const o = new Uint32Array(32);
  o[0] = o[1] = o[4] = 256;
  o[2] = 1;
  return o;
};
export const KEY: Record<string, number> = {
  KeyZ: 1,
  Enter: 1,
  Space: 1,
  KeyX: 2,
  Backspace: 2,
  Tab: 4,
  ShiftLeft: 4,
  Escape: 8,
  ArrowRight: 16,
  KeyD: 16,
  ArrowLeft: 32,
  KeyA: 32,
  ArrowUp: 64,
  KeyW: 64,
  ArrowDown: 128,
  KeyS: 128,
  KeyE: 256,
  KeyQ: 512,
  KeyC: 1024,
  KeyV: 2048,
};
