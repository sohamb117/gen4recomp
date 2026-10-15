import { nativeGames } from "./native-games.mjs";
export const gen123Games = [
  "red",
  "blue",
  "yellow",
  "gold",
  "silver",
  "crystal",
  "firered",
  "leafgreen",
  "emerald",
];
export const gen4Games = [
  "diamond",
  "pearl",
  "platinum",
  "heartgold",
  "soulsilver",
  "black",
  "white",
];
export const allSaveGames = [
  ...new Set([...gen123Games, ...Object.keys(nativeGames)]),
];
export const MAX_SAVE_BYTES = 4 * 1024 * 1024;
/** Gen 1–3 use engine saves, never a raw cartridge SRAM image or executable Lua. */
export function readEngineSave(bytes, game) {
  if (
    !gen123Games.includes(game) ||
    !bytes.byteLength ||
    bytes.byteLength > MAX_SAVE_BYTES
  )
    throw Error("Invalid engine save size or game.");
  const value = JSON.parse(
    new TextDecoder("utf-8", { fatal: true }).decode(bytes),
  );
  if (
    value?.format !== "pokeweb-gen123-v1" ||
    value.game !== game ||
    typeof value.save !== "string" ||
    !value.save.startsWith("return {") ||
    (value.options !== undefined &&
      (typeof value.options !== "string" ||
        !value.options.startsWith("return {")))
  )
    throw Error("Choose a pokeweb engine save for this game (.pws).");
  // The engine subsequently validates its restricted data-only Lua grammar.
  return value;
}
export function validateSave(bytes, game) {
  if (game === "emerald" && bytes.byteLength !== 131072)
    return readEngineSave(bytes, game);
  if (game in nativeGames) {
    if (bytes.byteLength !== nativeGames[game].saveBytes)
      throw Error(`Save must be ${nativeGames[game].saveBytes / 1024} KiB.`);
  } else readEngineSave(bytes, game);
}
