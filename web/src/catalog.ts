import { nativeGames, type NativeGameId } from "../shared/native-games.mjs";
export type Gen4GameId = "diamond" | "pearl" | "platinum";
export type Gen123GameId =
  | "red"
  | "blue"
  | "yellow"
  | "gold"
  | "silver"
  | "crystal"
  | "firered"
  | "leafgreen"
  | "emerald";
export type GameId = Gen4GameId | Gen123GameId | NativeGameId;
export function isNative(id: GameId): id is NativeGameId {
  return id in nativeGames;
}
export function isSingleScreen(id: GameId) {
  return isNative(id) && nativeGames[id].generation === 3;
}
export function usesLegacySave(id: GameId, data?: ArrayBuffer) {
  return (
    !isNative(id) || (id === "emerald" && !!data && data.byteLength !== 131072)
  );
}
export function saveExtension(id: GameId, data?: ArrayBuffer) {
  return usesLegacySave(id, data) ? "pws" : "sav";
}
export function isGen4(id: GameId): id is Gen4GameId {
  return id === "diamond" || id === "pearl" || id === "platinum";
}
export function generation(id: GameId): 1 | 2 | 3 | 4 | 5 {
  if (isNative(id)) return nativeGames[id].generation;
  if (["red", "blue", "yellow"].includes(id)) return 1;
  if (["gold", "silver", "crystal"].includes(id)) return 2;
  return 3;
}
export const games = [
  {
    id: "red" as const,
    name: "Red",
    code: "USA",
    year: "1998",
    color: "#cf8888",
    symbol: "○",
    hash: "ea9bcae617fdf159b045185467ae58b2e4a48b9a",
    description: "",
  },
  {
    id: "blue" as const,
    name: "Blue",
    code: "USA",
    year: "1998",
    color: "#93b4c5",
    symbol: "○",
    hash: "d7037c83e1ae5b39bde3c30787637ba1d4c48ce2",
    description: "",
  },
  {
    id: "yellow" as const,
    name: "Yellow",
    code: "USA",
    year: "1999",
    color: "#d9c977",
    symbol: "○",
    hash: "cc7d03262ebfaf2f06772c1a480c7d9d5f4a38e1",
    description: "",
  },
  {
    id: "gold" as const,
    name: "Gold",
    code: "USA",
    year: "2000",
    color: "#c8b06d",
    symbol: "○",
    hash: "d8b8a3600a465308c9953dfa04f0081c05bdcb94",
    description: "",
  },
  {
    id: "silver" as const,
    name: "Silver",
    code: "USA",
    year: "2000",
    color: "#b4bdc6",
    symbol: "○",
    hash: "49b163f7e57702bc939d642a18f591de55d92dae",
    description: "",
  },
  {
    id: "crystal" as const,
    name: "Crystal",
    code: "USA",
    year: "2001",
    color: "#a7addb",
    symbol: "○",
    hash: "f4cd194bdee0d04ca4eac29e09b8e4e9d818c133",
    description: "",
  },
  {
    id: "firered" as const,
    name: "FireRed",
    code: "USA",
    year: "2004",
    color: "#cc9972",
    symbol: "○",
    hash: "41cb23d8dccc8ebd7c649cd8fbb58eeace6e2fdc",
    description: "",
  },
  {
    id: "leafgreen" as const,
    name: "LeafGreen",
    code: "USA",
    year: "2004",
    color: "#9fbd88",
    symbol: "○",
    hash: "574fa542ffebb14be69902d1d36f1ec0a4afd71e",
    description: "",
  },
  {
    id: "emerald" as const,
    name: "Emerald",
    code: "USA",
    year: "2005",
    color: "#80b79e",
    symbol: "○",
    hash: "f3ae088181bf583e55daf962a92bb46f4f1d07b7",
    description: "",
  },
  {
    id: "diamond" as const,
    name: "Diamond",
    code: "ADA / USA",
    year: "2007",
    color: "#93b4c5",
    symbol: "◇",
    hash: "a46233d8b79a69ea87aa295a0efad5237d02841e",
    description: "A new beginning. A world of possibility.",
  },
  {
    id: "pearl" as const,
    name: "Pearl",
    code: "APA / USA",
    year: "2007",
    color: "#c39aaa",
    symbol: "◉",
    hash: "99083bf15ec7c6b81b4ba241ee10abd9e80999ac",
    description: "Another perspective on the same distant world.",
  },
  {
    id: "platinum" as const,
    name: "Platinum",
    code: "CPU / USA REV 1",
    year: "2009",
    color: "#bfc3a0",
    symbol: "✧",
    hash: "0862ec35b24de5c7e2dcb88c9eea0873110d755c",
    description: "Somewhere between this world and the next.",
  },
  ...(
    ["ruby", "sapphire", "heartgold", "soulsilver", "black", "white"] as const
  ).map((id) => ({
    id,
    name: nativeGames[id].name,
    code: "USA",
    year: {
      ruby: "2003",
      sapphire: "2003",
      heartgold: "2010",
      soulsilver: "2010",
      black: "2011",
      white: "2011",
    }[id],
    color: {
      ruby: "#cf8888",
      sapphire: "#93b4c5",
      heartgold: "#c8b06d",
      soulsilver: "#b4bdc6",
      black: "#9ca2af",
      white: "#d4d8ce",
    }[id],
    symbol: "◇",
    hash: nativeGames[id].sha1,
    description: "",
  })),
];
export type CoreManifest = {
  version: number;
  abi: number;
  runtime: string;
  games: Partial<
    Record<
      GameId,
      {
        file: string;
        bytes: number;
        sha256: string;
        sourceSha256: string;
        verification?: "passed" | "blocked" | "unverified";
        note?: string;
      }
    >
  >;
};
export async function identifyRom(data: ArrayBuffer) {
  if (data.byteLength < 512 || data.byteLength > 256 * 1024 * 1024)
    throw Error(
      "Choose a cartridge dump (.gb, .gbc, .gba, or .nds, up to 256 MB).",
    );
  const hash = [...new Uint8Array(await crypto.subtle.digest("SHA-1", data))]
    .map((x) => x.toString(16).padStart(2, "0"))
    .join("");
  const revisions: Record<string, GameId> = {
    f2f52230b536214ef7c9924f483392993e226cfb: "crystal",
    dd5945db9b930750cb39d00c84da8571feebf417: "firered",
    "7862c67bdecbe21d1d69ce082ce34327e1c6ed5e": "leafgreen",
  };
  const game = games.find((g) => g.hash === hash || g.id === revisions[hash]);
  if (!game)
    throw Error(
      hash === "ce81046eda7d232513069519cb2085349896dec7"
        ? "This is Platinum Rev 0. Please use the USA Rev 1 cartridge."
        : `Cartridge not recognized. Use a matching USA dump. SHA-1: ${hash}`,
    );
  return game;
}
