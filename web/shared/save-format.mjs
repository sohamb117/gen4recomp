export const gen123Games = ['red', 'blue', 'yellow', 'gold', 'silver', 'crystal', 'firered', 'leafgreen', 'emerald'];
export const gen4Games = ['diamond', 'pearl', 'platinum'];
export const MAX_SAVE_BYTES = 4 * 1024 * 1024;
/** Gen 1–3 use engine saves, never a raw cartridge SRAM image or executable Lua. */
export function readEngineSave(bytes, game) {
  if (!gen123Games.includes(game) || !bytes.byteLength || bytes.byteLength > MAX_SAVE_BYTES)
    throw Error('Invalid engine save size or game.');
  const value = JSON.parse(new TextDecoder('utf-8', {fatal: true}).decode(bytes));
  if (value?.format !== 'pokeweb-gen123-v1' || value.game !== game ||
      typeof value.save !== 'string' || !value.save.startsWith('return {') ||
      (value.options !== undefined && (typeof value.options !== 'string' || !value.options.startsWith('return {'))))
    throw Error('Choose a pokeweb engine save for this game (.pws).');
  // The engine subsequently validates its restricted data-only Lua grammar.
  return value;
}
export function validateSave(bytes, game) {
  if (gen4Games.includes(game)) {
    if (bytes.byteLength !== 524288) throw Error('DS saves must be 512 KiB.');
  } else readEngineSave(bytes, game);
}
