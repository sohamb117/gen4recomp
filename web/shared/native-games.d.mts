export type NativeGameId =
  | "diamond"
  | "pearl"
  | "platinum"
  | "heartgold"
  | "soulsilver"
  | "ruby"
  | "sapphire"
  | "emerald"
  | "black"
  | "white";
export const nativeGames: Record<
  NativeGameId,
  {
    name: string;
    generation: 3 | 4 | 5;
    saveBytes: number;
    sha1: string;
    rom: string;
  }
>;
