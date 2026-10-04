export type GameId = "diamond" | "pearl" | "platinum";
export const games = [
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
    throw Error("Choose a Nintendo DS cartridge dump (.nds, up to 256 MB).");
  const hash = [...new Uint8Array(await crypto.subtle.digest("SHA-1", data))]
    .map((x) => x.toString(16).padStart(2, "0"))
    .join("");
  const game = games.find((g) => g.hash === hash);
  if (!game)
    throw Error(
      hash === "ce81046eda7d232513069519cb2085349896dec7"
        ? "This is Platinum Rev 0. Please use the USA Rev 1 cartridge."
        : `Cartridge not recognized. Use a matching USA dump. SHA-1: ${hash}`,
    );
  return game;
}
