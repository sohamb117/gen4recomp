import { nativeGames } from "../shared/native-games.mjs";
import { validateSave } from "../shared/save-format.mjs";
import { isGen4, isNative, usesLegacySave, type GameId } from "./catalog";
export type Cartridge = {
  game: GameId;
  name: string;
  data: ArrayBuffer;
  imported: number;
};
export type SaveSlot = {
  id: string;
  game: GameId;
  name: string;
  updated: number;
  data?: ArrayBuffer;
  backup?: ArrayBuffer;
};
let pending: Promise<IDBDatabase> | undefined;
function database() {
  return (pending ??= new Promise((resolve, reject) => {
    const r = indexedDB.open("nativeplat-wired", 1);
    r.onupgradeneeded = () => {
      r.result.createObjectStore("roms", { keyPath: "game" });
      r.result.createObjectStore("slots", { keyPath: "id" });
    };
    r.onsuccess = () => resolve(r.result);
    r.onerror = () => {
      pending = undefined;
      reject(r.error);
    };
  }));
}
async function request<T>(
  store: string,
  mode: IDBTransactionMode,
  act: (store: IDBObjectStore) => IDBRequest<T>,
): Promise<T> {
  const db = await database();
  return new Promise((resolve, reject) => {
    const tx = db.transaction(store, mode);
    const r = act(tx.objectStore(store));
    tx.oncomplete = () => resolve(r.result);
    tx.onerror = () => reject(tx.error);
    tx.onabort = () => reject(tx.error ?? Error("Storage write aborted"));
  });
}
type StoredCartridge = Omit<Cartridge, "data"> & { data: ArrayBuffer | Blob };
async function decodeCartridge(
  stored: StoredCartridge | undefined,
): Promise<Cartridge | undefined> {
  if (!stored) return undefined;
  return {
    ...stored,
    data:
      stored.data instanceof Blob
        ? await stored.data.arrayBuffer()
        : stored.data,
  };
}
export const storage = {
  cartridgeIds: () =>
    request("roms", "readonly", (s) => s.getAllKeys()) as Promise<GameId[]>,
  cartridges: async () => {
    const records = await request<StoredCartridge[]>("roms", "readonly", (s) =>
      s.getAll(),
    );
    return Promise.all(
      records.map(async (record) => (await decodeCartridge(record))!),
    );
  },
  cartridge: async (id: GameId) =>
    decodeCartridge(
      await request<StoredCartridge | undefined>("roms", "readonly", (s) =>
        s.get(id),
      ),
    ),
  // Blob payloads are stored out of line by IndexedDB. Large ArrayBuffers can
  // exceed Chromium's per-record serialization limit even with ample quota.
  importRom: (rom: Cartridge) =>
    request("roms", "readwrite", (s) =>
      s.put({ ...rom, data: new Blob([rom.data]) }),
    ),
  slots: () =>
    request("slots", "readonly", (s) => s.getAll()) as Promise<SaveSlot[]>,
  putSlot: (slot: SaveSlot) =>
    request("slots", "readwrite", (s) => s.put(slot)),
  async save(id: string, data: ArrayBuffer) {
    const db = await database();
    return new Promise<void>((resolve, reject) => {
      const tx = db.transaction("slots", "readwrite"),
        s = tx.objectStore("slots"),
        r = s.get(id);
      r.onsuccess = () => {
        if (!r.result) {
          tx.abort();
          return;
        }
        const old = r.result as SaveSlot;
        s.put({ ...old, data, backup: old.data, updated: Date.now() });
      };
      tx.oncomplete = () => resolve();
      tx.onerror = () => reject(tx.error);
      tx.onabort = () =>
        reject(Error("Save could not be committed to browser storage"));
    });
  },
};
export function saveData(data: ArrayBuffer, game: GameId = "platinum") {
  if (usesLegacySave(game, data)) {
    validateSave(new Uint8Array(data), game);
    return data.slice(0);
  }
  const bytes = isNative(game) ? nativeGames[game].saveBytes : 524288;
  if (
    data.byteLength !== bytes &&
    !(bytes === 524288 && data.byteLength === bytes + 122)
  )
    throw Error(
      `Save must be ${bytes / 1024} KiB${bytes === 524288 ? " raw .sav or a DeSmuME .dsv with its 122-byte footer" : " raw .sav"}.`,
    );
  return data.slice(0, bytes);
}
export function download(
  data: ArrayBuffer,
  name: string,
  type = "application/octet-stream",
) {
  const url = URL.createObjectURL(new Blob([data], { type }));
  const a = document.createElement("a");
  a.href = url;
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
