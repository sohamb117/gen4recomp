import {
  mkdirSync,
  readFileSync,
  writeFileSync,
  renameSync,
  rmSync,
} from "node:fs";
/** Build/test coordination only; never part of the browser frame loop. */
export function updateCoreManifest(path, update, initial) {
  const lock = path + ".lock",
    deadline = Date.now() + 30000;
  while (true) {
    try {
      mkdirSync(lock);
      break;
    } catch (e) {
      if (e.code !== "EEXIST" || Date.now() > deadline) throw e;
      Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 10);
    }
  }
  try {
    let current;
    try {
      current = JSON.parse(readFileSync(path, "utf8"));
    } catch (e) {
      if (e.code !== "ENOENT") throw e;
      current = initial;
    }
    update(current);
    const tmp = path + `.${process.pid}.tmp`;
    writeFileSync(tmp, JSON.stringify(current, null, 2) + "\n");
    renameSync(tmp, path);
  } finally {
    rmSync(lock, { recursive: true });
  }
}
