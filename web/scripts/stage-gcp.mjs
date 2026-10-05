import { stageGen123 } from "./gen123-package.mjs";
// Explicit deployment bundle: verified engines and twelve allowlisted cartridges.
import { createHash } from "node:crypto";
import {
  readFileSync,
  writeFileSync,
  mkdirSync,
  rmSync,
  cpSync,
  readdirSync,
  statSync,
} from "node:fs";
import { resolve, relative } from "node:path";
import { gzipSync } from "node:zlib";
import { packCartridge } from "./cartridge-package.mjs";

const root = resolve(import.meta.dirname, "../..");
const dist = resolve(root, "web/dist");
const stage = resolve(root, "build/gcp-nativeplat");
const site = resolve(stage, "dist");
const dsGames = ["diamond", "pearl", "platinum"];
const selected = {
  red: [
    "build/gen123-roms/pokered/pokered.gbc",
    "ea9bcae617fdf159b045185467ae58b2e4a48b9a",
  ],
  blue: [
    "build/gen123-roms/pokered/pokeblue.gbc",
    "d7037c83e1ae5b39bde3c30787637ba1d4c48ce2",
  ],
  yellow: [
    "build/gen123-roms/pokeyellow/pokeyellow.gbc",
    "cc7d03262ebfaf2f06772c1a480c7d9d5f4a38e1",
  ],
  gold: [
    "build/gen123-roms/pokegold/pokegold.gbc",
    "d8b8a3600a465308c9953dfa04f0081c05bdcb94",
  ],
  silver: [
    "build/gen123-roms/pokegold/pokesilver.gbc",
    "49b163f7e57702bc939d642a18f591de55d92dae",
  ],
  crystal: [
    "build/gen123-roms/pokecrystal/pokecrystal.gbc",
    "f4cd194bdee0d04ca4eac29e09b8e4e9d818c133",
  ],
  firered: [
    "build/gen123-roms/pokefirered/pokefirered.gba",
    "41cb23d8dccc8ebd7c649cd8fbb58eeace6e2fdc",
  ],
  leafgreen: [
    "build/gen123-roms/pokefirered/pokeleafgreen.gba",
    "574fa542ffebb14be69902d1d36f1ec0a4afd71e",
  ],
  emerald: [
    "build/gen123-roms/pokeemerald/pokeemerald.gba",
    "f3ae088181bf583e55daf962a92bb46f4f1d07b7",
  ],

  diamond: [
    "games/diamond/build/diamond.us/pokediamond.us.nds",
    "a46233d8b79a69ea87aa295a0efad5237d02841e",
  ],
  pearl: [
    "games/diamond/build/pearl.us/pokepearl.us.nds",
    "99083bf15ec7c6b81b4ba241ee10abd9e80999ac",
  ],
  platinum: [
    "games/platinum/build/rom/pokeplatinum.us.nds",
    "0862ec35b24de5c7e2dcb88c9eea0873110d755c",
  ],
};
const hash = (data, algorithm = "sha256") =>
  createHash(algorithm).update(data).digest("hex");
const cores = JSON.parse(
  readFileSync(resolve(dist, "cores/manifest.json"), "utf8"),
);
const cartridges = { version: 2, games: {} };
rmSync(stage, { recursive: true, force: true });
mkdirSync(resolve(site, "cores"), { recursive: true });
mkdirSync(resolve(site, "cartridges"));
stageGen123(resolve(dist, "engines/gen123"), resolve(site, "engines/gen123"));
// Allowlist site outputs, never the repository or user storage.
for (const name of ["index.html", "icon.svg", "assets"])
  cpSync(resolve(dist, name), resolve(site, name), { recursive: true });
for (const game of dsGames) {
  const core = cores.games[game];
  if (
    !core ||
    core.verification !== "passed" ||
    !/^[a-z0-9-]+\.wasm$/.test(core.file)
  )
    throw Error(`${game} has no verified web core`);
  const wasm = readFileSync(resolve(dist, "cores", core.file));
  if (wasm.length !== core.bytes || hash(wasm) !== core.sha256)
    throw Error(`${game} core integrity mismatch`);
  writeFileSync(resolve(site, "cores", core.file), wasm);
}
for (const [game, [source, sha1]] of Object.entries(selected)) {
  const data = readFileSync(resolve(root, source));
  if (hash(data, "sha1") !== sha1)
    throw Error(`${game} cartridge integrity mismatch`);
  const { payload, entry } = packCartridge(game, data);
  writeFileSync(resolve(site, "cartridges", entry.file), payload);
  cartridges.games[game] = entry;
}
cores.games = Object.fromEntries(
  dsGames.map((game) => [game, cores.games[game]]),
);
writeFileSync(
  resolve(site, "cores/manifest.json"),
  JSON.stringify(cores, null, 2) + "\n",
);
writeFileSync(
  resolve(site, "cartridges/manifest.json"),
  JSON.stringify(cartridges, null, 2) + "\n",
);
cpSync(
  resolve(root, "games/platinum/pc/LICENSE"),
  resolve(site, "LICENSE.txt"),
);
cpSync(resolve(root, "web/THIRD-PARTY.txt"), resolve(site, "THIRD-PARTY.txt"));
const sums = [];
function walk(dir) {
  for (const name of readdirSync(dir)) {
    const file = resolve(dir, name);
    if (statSync(file).isDirectory()) {
      walk(file);
      continue;
    }
    if (/\.(nds|gb|gbc|gba|sav|dsv|pws)(\.|$)/i.test(name))
      throw Error(`Raw cartridge/save in GCP bundle: ${name}`);
    const data = readFileSync(file);
    sums.push(`${hash(data)}  ${relative(site, file)}`);
    if (data.length > 1024 && !name.endsWith(".npc"))
      writeFileSync(file + ".gz", gzipSync(data, { level: 9 }));
  }
}
walk(site);
writeFileSync(resolve(site, "SHA256SUMS"), sums.sort().join("\n") + "\n");
const withApi = process.env.NP_WITH_SAVE_API === "1";
cpSync(
  resolve(
    root,
    withApi ? "web/deploy/Dockerfile.cloud" : "web/deploy/Dockerfile",
  ),
  resolve(stage, "Dockerfile"),
);
if (withApi) {
  cpSync(resolve(root, "web/shared"), resolve(stage, "shared"), {
    recursive: true,
  });
  mkdirSync(resolve(stage, "server"));
  for (const file of [
    "package.json",
    "package-lock.json",
    "app.mjs",
    "index.mjs",
    "schema.sql",
  ])
    cpSync(resolve(root, "web/server", file), resolve(stage, "server", file));
}
mkdirSync(resolve(stage, "deploy"));
let nginx = readFileSync(resolve(root, "web/deploy/nginx.conf"), "utf8")
  .replace("wasm|js|css", "wasm|js|css|npc")
  .replace(
    "location / {",
    "location ~* \\.(nds|gb|gbc|gba|sav|dsv|pws)(\\.|$) { return 404; }\n    location / {",
  )
  // Cloud Run must also use --use-http2 for cartridge responses over 32 MiB.
  .replace("listen 8080;", "listen 8080;\n    http2 on;");
if (withApi)
  nginx = nginx.replace(
    "location / {",
    `location /api/ {
      client_max_body_size 6m;
      proxy_pass http://127.0.0.1:8081;
      proxy_set_header Host $host;
      proxy_set_header X-Pokeweb-Client-IP $np_client_ip;
      proxy_read_timeout 25s;
      proxy_connect_timeout 5s;
    }
    location / {`,
  );
const clientMap = `map $http_x_forwarded_for $np_client_ip {
    default $remote_addr;
    ~(?<last_ip>[^,\\s]+)$ $last_ip;
}
`;
writeFileSync(
  resolve(stage, "deploy/nginx.conf"),
  (withApi ? clientMap : "") + nginx,
);
writeFileSync(resolve(stage, ".gcloudignore"), ".git\nnode_modules\n");
console.log(
  `GCP context: ${stage}\nGames: ${Object.keys(selected).join(", ")}\nFiles: ${sums.length}`,
);
