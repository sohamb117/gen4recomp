import { games, identifyRom, type GameId } from "./catalog";

export type HostedCartridge = {
  file: string;
  bytes: number;
  sha256: string;
  packedBytes: number;
  packedSha256: string;
  encoding: "aes-256-gcm+gzip";
  key: string;
  iv: string;
};
export type HostedCartridges = Partial<Record<GameId, HostedCartridge>>;
const limit = 256 * 1024 * 1024;
const hex = (value: unknown, length: number): value is string =>
  typeof value === "string" && new RegExp(`^[a-f0-9]{${length}}$`).test(value);
const unhex = (value: string) => Uint8Array.from(value.match(/../g)!, (v) => parseInt(v, 16));
const digest = async (data: ArrayBuffer) =>
  [...new Uint8Array(await crypto.subtle.digest("SHA-256", data))]
    .map((n) => n.toString(16).padStart(2, "0")).join("");

export function parseCartridges(value: unknown): HostedCartridges {
  const m = value as { version?: unknown; games?: Record<string, HostedCartridge> };
  if (!m || m.version !== 2 || !m.games || typeof m.games !== "object" || Array.isArray(m.games))
    throw Error("Invalid hosted cartridge catalog. Reload the page to update it.");
  const result: HostedCartridges = {};
  for (const game of games) {
    const entry = m.games[game.id];
    if (!entry) continue;
    if (typeof entry.file !== "string" || !/^[a-z0-9-]+\.npc$/.test(entry.file) ||
        !Number.isSafeInteger(entry.bytes) || entry.bytes < 512 || entry.bytes > limit ||
        !Number.isSafeInteger(entry.packedBytes) || entry.packedBytes < 17 || entry.packedBytes > limit + 1024 * 1024 ||
        !hex(entry.sha256, 64) || !hex(entry.packedSha256, 64) ||
        entry.encoding !== "aes-256-gcm+gzip" || !hex(entry.key, 64) || !hex(entry.iv, 24))
      throw Error(`Invalid hosted cartridge: ${game.name}.`);
    result[game.id] = { ...entry };
  }
  return result;
}

export async function loadCartridges(base: string): Promise<HostedCartridges> {
  const response = await fetch(`${base}cartridges/manifest.json`);
  if (response.status === 404) return {};
  if (!response.ok) throw Error("Hosted cartridges are unavailable. Try again later.");
  return parseCartridges(await response.json());
}

async function readBounded(stream: ReadableStream<Uint8Array>, size: number, progress: (fraction: number) => void) {
  const reader = stream.getReader();
  const data = new Uint8Array(size);
  let offset = 0;
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      if (offset + value.length > size) throw Error("Cartridge exceeded its expected size.");
      data.set(value, offset);
      offset += value.length;
      progress(offset / size);
    }
    if (offset !== size) throw Error("Cartridge was incomplete. Please retry.");
    return data.buffer;
  } catch (error) {
    await reader.cancel().catch(() => {});
    throw error;
  } finally {
    reader.releaseLock();
  }
}

/** Distribution obfuscation only: the public catalog deliberately supplies the key. */
export async function unpackCartridge(data: ArrayBuffer, entry: HostedCartridge): Promise<ArrayBuffer> {
  parseCartridges({ version: 2, games: { diamond: entry } });
  if (data.byteLength !== entry.packedBytes || await digest(data) !== entry.packedSha256)
    throw Error("Cartridge package integrity check failed.");
  const key = await crypto.subtle.importKey("raw", unhex(entry.key), "AES-GCM", false, ["decrypt"]);
  let compressed: ArrayBuffer;
  try {
    compressed = await crypto.subtle.decrypt({ name: "AES-GCM", iv: unhex(entry.iv), tagLength: 128 }, key, data);
  } catch {
    throw Error("Cartridge package could not be decrypted. Reload and retry.");
  }
  const stream = new Blob([compressed]).stream().pipeThrough(new DecompressionStream("gzip"));
  const plain = await readBounded(stream, entry.bytes, () => {});
  if (await digest(plain) !== entry.sha256) throw Error("Cartridge integrity check failed.");
  return plain;
}

export async function fetchCartridge(
  base: string, game: GameId, entry: HostedCartridge,
  progress: (fraction: number) => void,
): Promise<ArrayBuffer> {
  parseCartridges({ version: 2, games: { [game]: entry } });
  const response = await fetch(`${base}cartridges/${entry.file}`);
  if (!response.ok || !response.body) throw Error("Cartridge download failed. Please retry.");
  const packed = await readBounded(response.body, entry.packedBytes, progress);
  const plain = await unpackCartridge(packed, entry);
  if ((await identifyRom(plain)).id !== game) throw Error("Downloaded cartridge does not match the selected game.");
  return plain;
}
