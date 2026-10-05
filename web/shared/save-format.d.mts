export const gen123Games: string[];
export const gen4Games: string[];
export const MAX_SAVE_BYTES: number;
export function readEngineSave(
  bytes: Uint8Array,
  game: string,
): {
  format: "pokeweb-gen123-v1";
  game: string;
  save: string;
  options?: string;
};
export function validateSave(bytes: Uint8Array, game: string): void;
