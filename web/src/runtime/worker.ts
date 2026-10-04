/// <reference lib="webworker" />
import { GuestHost } from "./host";
import type { Input } from "./abi";
let guest: GuestHost | undefined;
self.onmessage = async ({ data }) => {
  try {
    if (data.type === "load") {
      const response = await fetch(data.url);
      if (!response.ok)
        throw Error(`Core download failed (${response.status})`);
      const wasm = await response.arrayBuffer();
      const hash = [
        ...new Uint8Array(await crypto.subtle.digest("SHA-256", wasm)),
      ]
        .map((x) => x.toString(16).padStart(2, "0"))
        .join("");
      if (hash !== data.sha256)
        throw Error(
          "Core integrity check failed. Reload the page to get the current release.",
        );
      guest = new GuestHost(
        new Uint8Array(data.rom),
        data.save ? new Uint8Array(data.save) : undefined,
        (save) => {
          const copy = save.slice().buffer;
          self.postMessage({ type: "save", data: copy }, [copy]);
        },
        (text) => self.postMessage({ type: "log", text }),
      );
      await guest.load(wasm);
      self.postMessage({ type: "ready" });
    } else if (data.type === "frame" && guest) {
      guest.input = data.input as Input;
      guest.options.set(data.options);
      const speed = Math.max(1, Math.min(6, Number(data.speed) || 1));
      const started = performance.now();
      const frame = guest.runFrame();
      const workerMs = performance.now() - started;
      self.postMessage({ type: "frame", frame, ticks: 1, speed, workerMs }, [
        frame.top.buffer,
        frame.bottom.buffer,
        frame.audio.buffer,
      ]);
    } else if (data.type === "flush" && guest) {
      guest.flush();
      self.postMessage({ type: "flushed" });
    }
  } catch (error) {
    self.postMessage({
      type: "error",
      message: error instanceof Error ? error.message : String(error),
    });
  }
};
