import { test } from "node:test";
import assert from "node:assert/strict";
import "fake-indexeddb/auto";
import { CloudStore } from "../src/cloud/client";
import { storage, type SaveSlot } from "../src/storage";
const entries = new Map<string, string>();
Object.defineProperty(globalThis, "localStorage", {
  value: {
    getItem: (key: string) => entries.get(key) ?? null,
    setItem: (key: string, value: string) => entries.set(key, value),
  },
  configurable: true,
});
const user = { id: "test-account", username: "player" };
const saved = (revision = 1) => ({
  game: "platinum",
  name: "Journey",
  revision,
  updated: Date.now(),
  sha256: "a".repeat(64),
});
function slot(id = crypto.randomUUID(), byte = 1): SaveSlot {
  return {
    id,
    game: "platinum",
    name: "Journey",
    data: new Uint8Array(524288).fill(byte).buffer,
    updated: Date.now(),
  };
}
function settled(store: CloudStore) {
  if (!store.snapshot().busy) return Promise.resolve();
  return new Promise<void>((resolve, reject) => {
    const timeout = setTimeout(() => {
      off();
      reject(Error("Cloud operation timed out"));
    }, 5000);
    const off = store.subscribe(() => {
      if (!store.snapshot().busy) {
        clearTimeout(timeout);
        off();
        resolve();
      }
    });
  });
}
function respond(body: unknown, status = 200) {
  return new Response(JSON.stringify(body), { status });
}
test("local play never uploads without login; signed-in saves coalesce without blocking local persistence", async () => {
  entries.clear();
  const original = globalThis.fetch;
  const uploads: any[] = [];
  let release!: () => void;
  const gate = new Promise<void>((r) => {
    release = r;
  });
  globalThis.fetch = async (_url, init) => {
    if (init?.method === "POST") return respond({ user, save: null });
    const body = JSON.parse(String(init?.body));
    uploads.push(body);
    if (uploads.length === 1) await gate;
    return respond({ save: { ...saved(uploads.length), sha256: body.sha256 } });
  };
  try {
    const store = new CloudStore(),
      first = slot();
    await storage.putSlot(first);
    store.saved(first);
    assert.equal(uploads.length, 0);
    await store.authenticate("player", "password123", false);
    store.saved(first);
    const newer = { ...first, data: slot(undefined, 2).data };
    store.saved(newer);
    await storage.save(first.id, newer.data!);
    assert.equal(
      store.snapshot().busy,
      true,
      "network is still pending while local save completed",
    );
    release();
    await settled(store);
    assert.equal(uploads.length, 2);
    assert.equal(uploads[1].revision, 1);
    assert.equal(uploads[1].accountId, user.id);
    assert.equal(atob(uploads[1].data).charCodeAt(0), 2);
    assert.equal(store.snapshot().save?.revision, 2);
  } finally {
    globalThis.fetch = original;
  }
});
test("conflicting remote writes stop automatic retries and preserve the local save", async () => {
  entries.clear();
  const original = globalThis.fetch;
  let puts = 0;
  globalThis.fetch = async (_url, init) => {
    if (init?.method === "POST") return respond({ user, save: null });
    puts++;
    return respond({ error: "Cloud save changed on another device." }, 409);
  };
  try {
    const store = new CloudStore(),
      local = slot();
    await storage.putSlot(local);
    await store.authenticate("player", "password123", false);
    store.saved(local);
    await settled(store);
    assert.match(
      store.snapshot().status,
      /another device.*still on this device/,
    );
    store.saved(local);
    await store.retry();
    assert.equal(puts, 1);
    assert.deepEqual(
      (await storage.slots()).find((s) => s.id === local.id)?.data,
      local.data,
    );
  } finally {
    globalThis.fetch = original;
  }
});
test("loading cloud data checks integrity and creates a new local slot without replacing existing slots", async () => {
  entries.clear();
  const original = globalThis.fetch;
  const local = slot(),
    payload = slot();
  await storage.putSlot(local);
  const sha256 = Buffer.from(
    await crypto.subtle.digest("SHA-256", payload.data!),
  ).toString("hex");
  const remote = { ...saved(), sha256 };
  let corrupt = false;
  globalThis.fetch = async (_url, init) =>
    init?.method === "POST"
      ? respond({ user, save: remote })
      : respond({
          ...remote,
          accountId: user.id,
          sha256: corrupt ? "0".repeat(64) : sha256,
          data: Buffer.from(payload.data!).toString("base64"),
        });
  try {
    const store = new CloudStore();
    await store.authenticate("player", "password123", false);
    const loaded = await store.load();
    assert.ok(loaded);
    assert.notEqual(loaded.id, local.id);
    assert.deepEqual(loaded.data, payload.data);
    assert.ok((await storage.slots()).some((s) => s.id === local.id));
    corrupt = true;
    assert.equal(await store.load(), undefined);
    assert.match(store.snapshot().status, /integrity/);
  } finally {
    globalThis.fetch = original;
  }
});
