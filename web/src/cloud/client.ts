import { storage, saveData, type SaveSlot } from "../storage";
import type { GameId } from "../catalog";
export type CloudUser = { id: string; username: string };
export type CloudSave = {
  game: GameId;
  name: string;
  revision: number;
  updated: number;
  sha256: string;
};
export type CloudState = {
  user: CloudUser | null;
  save: CloudSave | null;
  busy: boolean;
  status: string;
  linkedId: string | null;
};
export class CloudError extends Error {
  constructor(
    public status: number,
    message: string,
  ) {
    super(message);
  }
}
export async function api<T>(
  path: string,
  method = "GET",
  body?: unknown,
): Promise<T> {
  const response = await fetch(`/api/${path}`, {
    method,
    credentials: "same-origin",
    cache: "no-store",
    headers: body ? { "Content-Type": "application/json" } : undefined,
    body: body ? JSON.stringify(body) : undefined,
    signal: AbortSignal.timeout(20000),
  });
  const result = await response
    .json()
    .catch(() => ({ error: "Cloud saves are unavailable on this host." }));
  if (!response.ok)
    throw new CloudError(
      response.status,
      result.error || "Cloud save request failed.",
    );
  return result as T;
}
async function digest(data: ArrayBuffer) {
  return Array.from(
    new Uint8Array(await crypto.subtle.digest("SHA-256", data)),
    (x) => x.toString(16).padStart(2, "0"),
  ).join("");
}
function encode(data: ArrayBuffer) {
  const bytes = new Uint8Array(data);
  let text = "";
  for (let i = 0; i < bytes.length; i += 8192)
    text += String.fromCharCode(...bytes.subarray(i, i + 8192));
  return btoa(text);
}
type Account = { user: CloudUser; save: CloudSave | null };
export class CloudStore {
  private state: CloudState = {
    user: null,
    save: null,
    busy: false,
    status: "",
    linkedId: null,
  };
  private listeners = new Set<() => void>();
  private pending: SaveSlot | null = null;
  private draining = false;
  private blocked = false;
  private revision = 0;
  private initialized = false;
  subscribe = (fn: () => void) => {
    this.listeners.add(fn);
    return () => {
      this.listeners.delete(fn);
    };
  };
  snapshot = () => this.state;
  private update(patch: Partial<CloudState>) {
    this.state = { ...this.state, ...patch };
    this.listeners.forEach((fn) => fn());
  }
  private remember() {
    if (!this.state.user) return;
    try {
      localStorage.setItem(
        `pokeweb-cloud-${this.state.user.id}`,
        JSON.stringify({
          slotId: this.state.linkedId,
          revision: this.revision,
        }),
      );
    } catch {
      /* Cloud data remains authoritative. */
    }
  }
  private async adopt(account: Account) {
    this.pending = null;
    this.blocked = false;
    this.revision = account.save?.revision || 0;
    let linkedId: string | null = null;
    try {
      const binding = JSON.parse(
        localStorage.getItem(`pokeweb-cloud-${account.user.id}`) || "null",
      );
      if (binding?.revision === this.revision) linkedId = binding.slotId;
    } catch {
      /* New device. */
    }
    this.update({
      ...account,
      linkedId,
      status: account.save
        ? linkedId
          ? "Cloud save connected."
          : "Load your cloud save to continue here."
        : "Your next in-game save will sync here.",
    });
    if (linkedId) {
      const local = (await storage.slots()).find((s) => s.id === linkedId);
      if (local?.data && (await digest(local.data)) !== account.save?.sha256)
        this.pending = local;
    }
  }
  async initialize() {
    if (this.initialized) return;
    this.initialized = true;
    this.update({ busy: true });
    try {
      await this.adopt(await api<Account>("account"));
    } catch (e) {
      if (!(e instanceof CloudError && e.status === 401))
        this.update({
          status:
            "Cloud saves are currently unavailable. Local play still works.",
        });
    }
    this.update({ busy: false });
    void this.drain();
  }
  async authenticate(username: string, password: string, create: boolean) {
    if (this.state.busy) return;
    this.update({ busy: true, status: "" });
    try {
      await this.adopt(
        await api<Account>(`auth/${create ? "register" : "login"}`, "POST", {
          username,
          password,
        }),
      );
    } catch (e) {
      this.update({ status: this.message(e) });
    } finally {
      this.update({ busy: false });
      void this.drain();
    }
  }
  async logout() {
    if (this.state.busy) return;
    this.update({ busy: true });
    try {
      await api("auth/logout", "POST", {});
      this.pending = null;
      this.blocked = false;
      this.revision = 0;
      this.update({ user: null, save: null, linkedId: null, status: "" });
    } catch (e) {
      this.update({ status: this.message(e) });
    } finally {
      this.update({ busy: false });
    }
  }
  private message(e: unknown) {
    return e instanceof Error ? e.message : String(e);
  }
  saved(slot: SaveSlot) {
    if (!this.state.user || !slot.data) return;
    if (!this.state.linkedId && !this.state.save) {
      this.update({ linkedId: slot.id });
      this.remember();
    }
    if (slot.id !== this.state.linkedId) {
      this.update({
        status: "Saved locally. Choose Save to cloud below to sync this slot.",
      });
      return;
    }
    this.pending = slot;
    // This never joins the game's save promise or frame loop.
    if (!this.state.busy && !this.blocked) void this.drain();
  }
  private async send(slot: SaveSlot, revision: number, accountId: string) {
    const data = saveData(slot.data!);
    const sha256 = await digest(data);
    return api<{ save: CloudSave }>("save", "PUT", {
      accountId,
      game: slot.game,
      name: slot.name,
      revision,
      sha256,
      data: encode(data),
    });
  }
  private async drain() {
    if (
      this.draining ||
      this.state.busy ||
      this.blocked ||
      !this.state.user ||
      !this.pending
    )
      return;
    this.draining = true;
    this.update({ busy: true, status: "Saving to cloud…" });
    try {
      while (this.pending && this.state.user) {
        const slot = this.pending;
        this.pending = null;
        try {
          const { save } = await this.send(
            slot,
            this.revision,
            this.state.user.id,
          );
          this.revision = save.revision;
          this.update({ save, status: "Saved to cloud." });
          this.remember();
        } catch (e) {
          this.pending ??= slot;
          throw e;
        }
      }
    } catch (e) {
      this.blocked =
        e instanceof CloudError && [401, 403, 409].includes(e.status);
      this.update({
        status: `${this.message(e)} Your latest save is still on this device.`,
      });
    } finally {
      this.draining = false;
      this.update({ busy: false });
    }
  }
  async retry() {
    if (!this.state.user || this.state.busy || this.blocked) return;
    if (!this.pending && this.state.linkedId)
      this.pending =
        (await storage.slots()).find((s) => s.id === this.state.linkedId) ||
        null;
    await this.drain();
  }
  async upload(slot: SaveSlot) {
    if (this.state.busy || !this.state.user || !slot.data) return;
    this.update({ busy: true, status: "Saving to cloud…" });
    try {
      // User explicitly selected Replace, but CAS still catches a concurrent writer.
      const latest = await api<Account>("account");
      if (latest.user.id !== this.state.user.id)
        throw Error("Account changed. Sign out and sign in again.");
      const { save } = await this.send(
        slot,
        latest.save?.revision || 0,
        this.state.user.id,
      );
      this.revision = save.revision;
      this.pending = null;
      this.blocked = false;
      this.update({
        save,
        linkedId: slot.id,
        status: "Saved to cloud. Future in-game saves sync automatically.",
      });
      this.remember();
      const current = (await storage.slots()).find((s) => s.id === slot.id);
      if (current?.data && (await digest(current.data)) !== save.sha256)
        this.pending = current;
    } catch (e) {
      this.update({
        status: `${this.message(e)} Your local save is unchanged.`,
      });
    } finally {
      this.update({ busy: false });
      void this.drain();
    }
  }
  async load(): Promise<SaveSlot | undefined> {
    if (this.state.busy || !this.state.user) return;
    this.update({ busy: true, status: "Loading cloud save…" });
    try {
      const remote = await api<CloudSave & { accountId: string; data: string }>(
        "save",
      );
      if (remote.accountId !== this.state.user.id)
        throw Error("Account changed. Sign out and sign in again.");
      const bytes = Uint8Array.from(atob(remote.data), (c) => c.charCodeAt(0));
      const data = saveData(bytes.buffer);
      if ((await digest(data)) !== remote.sha256)
        throw Error("Cloud save failed its integrity check.");
      const slot: SaveSlot = {
        id: crypto.randomUUID(),
        game: remote.game,
        name: remote.name,
        data,
        updated: Date.now(),
      };
      await storage.putSlot(slot);
      this.revision = remote.revision;
      this.pending = null;
      this.blocked = false;
      const { data: _, accountId: __, ...save } = remote;
      this.update({
        save,
        linkedId: slot.id,
        status: "Cloud save loaded. Choose Play to continue.",
      });
      this.remember();
      return slot;
    } catch (e) {
      this.update({ status: this.message(e) });
    } finally {
      this.update({ busy: false });
    }
  }
}
export const cloud = new CloudStore();
