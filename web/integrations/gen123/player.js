/* Isolated LÖVE process. Only its owning window can provide a cartridge. */
const channel = location.hash.slice(1);
let instance,
  started = false,
  pendingCommand;
const send = (message, transfers = []) =>
  parent.postMessage({ ...message, channel }, location.origin, transfers);
window.addEventListener("message", async ({ origin, source, data }) => {
  if (
    origin !== location.origin ||
    source !== parent ||
    data?.channel !== channel
  )
    return;
  if (data.type === "audio") {
    instance?.resumeAudio();
    return;
  }
  if (data.type === "command") {
    if (!data.command.paused && data.command.volume > 0)
      instance?.resumeAudio();
    pendingCommand = data.command;
    instance?.FS.writeFile("/command.json", JSON.stringify(data.command));
    return;
  }
  if (data.type === "save" || data.type === "flush") {
    instance?.FS.writeFile(
      "/request.json",
      JSON.stringify({ type: data.type, id: data.id }),
    );
    return;
  }
  if (data.type !== "start" || started) return;
  started = true;
  try {
    const manifestResponse = await fetch("manifest.json");
    if (!manifestResponse.ok) throw Error("Engine manifest unavailable");
    const manifest = await manifestResponse.json();
    const cacheKey = `${manifest.cacheVersion}:${data.game}:${data.sha1}`;
    let cached = await cacheRead(cacheKey);
    if (cached)
      for (const row of cached)
        if (row[1] instanceof Blob)
          row[1] = new Uint8Array(await row[1].arrayBuffer());
    const response = await fetch("game.love");
    if (!response.ok)
      throw Error(`Engine download failed (${response.status})`);
    let archive = new Uint8Array(await response.arrayBuffer());
    const actualHash = Array.from(
      new Uint8Array(await crypto.subtle.digest("SHA-256", archive)),
      (b) => b.toString(16).padStart(2, "0"),
    ).join("");
    if (actualHash !== manifest.files["game.love"].sha256)
      throw Error("Engine integrity check failed. Reload and retry.");
    const module = {
      arguments: ["/game.love"],
      INITIAL_MEMORY: 268435456,
      canvas: document.getElementById("canvas"),
      locateFile: (name) => name,
      print(line) {
        if (!line.startsWith("POKEWEB:")) {
          console.log(line);
          return;
        }
        const message = JSON.parse(line.slice(8));
        if (message.type === "cache") {
          const fs = instance.FS,
            files = [];
          function walk(path, relative) {
            if (!fs.analyzePath(path).exists) return;
            for (const name of fs.readdir(path)) {
              if (name === "." || name === "..") continue;
              const child = path + "/" + name,
                rel = relative + "/" + name;
              if (fs.isDir(fs.stat(child).mode)) walk(child, rel);
              else files.push([rel, fs.readFile(child)]);
            }
          }
          for (const dir of ["data/generated", "assets/generated"])
            walk(message.root + "/" + dir, dir);
          void cacheWrite(cacheKey, files);
        } else if (message.type === "save") {
          const fs = instance.FS;
          const save = new TextEncoder().encode(
            JSON.stringify({
              format: "pokeweb-gen123-v1",
              game: data.game,
              save: fs.readFile(message.path, { encoding: "utf8" }),
              options: fs.analyzePath(message.optionsPath).exists
                ? fs.readFile(message.optionsPath, { encoding: "utf8" })
                : undefined,
            }),
          ).buffer;
          send({ type: "save", data: save }, [save]);
        } else {
          if (message.type === "ready") instance?.resumeAudio();
          send(message);
        }
      },
      printErr: (...args) => console.error(...args),
      onAbort: (message) => send({ type: "error", message: String(message) }),
      preRun: [
        function () {
          instance = module;
          const fs = module.FS;
          fs.writeFile("/game.love", archive);
          if (cached)
            for (const [relative, bytes] of cached) {
              if (
                !/^(data|assets)\/generated\//.test(relative) ||
                relative.includes("..")
              )
                throw Error("Invalid extraction cache");
              const path = "/home/web_user/love/pokeweb-gen123/" + relative;
              fs.mkdirTree(path.slice(0, path.lastIndexOf("/")));
              fs.writeFile(path, bytes);
            }
          fs.writeFile("/cartridge.bin", new Uint8Array(data.rom));
          fs.writeFile(
            "/session.json",
            JSON.stringify({
              game: data.game,
              sha1: data.sha1,
              cached: !!cached,
            }),
          );
          if (data.save) fs.writeFile("/save.bin", data.save);
          if (data.options) fs.writeFile("/options.bin", data.options);
          if (pendingCommand)
            fs.writeFile("/command.json", JSON.stringify(pendingCommand));
          cached = null;
          archive = null;
          data.rom = null;
          data.save = null;
          data.options = null;
        },
      ],
    };
    await Love(module);
  } catch (error) {
    send({ type: "error", message: String(error) });
  }
});
send({ type: "loaded" });
