import { createCipheriv, createHash, randomBytes } from "node:crypto";
import { gzipSync } from "node:zlib";
const hash = (data) => createHash("sha256").update(data).digest("hex");

// The client receives this key. This format obscures direct ROM downloads;
// it is intentionally not an access-control or DRM boundary.
export function packCartridge(game, data) {
  const key = randomBytes(32), iv = randomBytes(12);
  const cipher = createCipheriv("aes-256-gcm", key, iv);
  const encrypted = cipher.update(gzipSync(data, { level: 9 }));
  const final = cipher.final();
  const payload = Buffer.concat([encrypted, final, cipher.getAuthTag()]);
  const packedSha256 = hash(payload);
  return {
    payload,
    entry: {
      file: `${game}-${packedSha256.slice(0, 16)}.npc`,
      bytes: data.length,
      sha256: hash(data),
      packedBytes: payload.length,
      packedSha256,
      encoding: "aes-256-gcm+gzip",
      key: key.toString("hex"),
      iv: iv.toString("hex"),
    },
  };
}
