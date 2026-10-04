# Public nativeplat deployment

Site: https://nativeplat-1066182835702.us-east1.run.app/

## Custom domain

The Cloud Run mapping for `pokeweb.morisoba.moe` points to `nativeplat` in
`us-east1`. Cloudflare manages DNS for `morisoba.moe`. Its required record is:

| Type | Name | Target | Proxy |
| --- | --- | --- | --- |
| CNAME | `pokeweb` | `ghs.googlehosted.com` | DNS only |

Google provisions and renews the HTTPS certificate after that record resolves.
Check readiness before using the custom address:

```sh
gcloud beta run domain-mappings describe --domain=pokeweb.morisoba.moe \
  --project=nativeplat-20261004 --region=us-east1
curl --fail --head https://pokeweb.morisoba.moe/
cd web
node --import tsx scripts/verify-deployment.ts https://pokeweb.morisoba.moe/
```

The existing `run.app` address remains available. Browser saves are scoped to
the origin: export a `.sav` there and import it at the custom domain to continue
an existing save. The domain mapping does not alter the game binaries or pacing.

## Service

- Project: `nativeplat-20261004`; region: `us-east1`; service: `nativeplat`.
- Cloud Run serves an nginx container from Artifact Registry `nativeplat/web`.
- Runtime identity: `nativeplat-web@nativeplat-20261004.iam.gserviceaccount.com`,
  with no project-level roles. The site has public invocation access.
- One CPU, 512 MiB RAM, concurrency 80, zero minimum / three maximum instances.
- Billing uses the same account as the previously selected `monereko-20260809` project.
- Encrypted-package image digest:
  `sha256:41f6c976b76a2fa3cc23df9c1938ae752e252e3461dbea6fc4de942b7d06ce6d`.
- Encrypted-package revision: `nativeplat-00002-dl6` (100% traffic).

The user explicitly selected public hosting of Diamond and Platinum. The
separate `scripts/stage-gcp.mjs` allowlists those exact local ROMs and verified
cores, checks their fingerprints, and writes only deployment assets into ignored
`build/gcp-nativeplat/`. Cartridges are gzip-compressed then AES-256-GCM encrypted
with fresh random keys and 96-bit IVs for each build, as hashed `.npc` files.
Raw `.nds` files are rejected by the staging audit and return 404 from nginx.
No saves, credentials, source checkout, Pearl ROM, or
unlisted WASM files enter the image. The ordinary portable release still
refuses ROM/save files. Nothing generated should be committed.

The frontend discovers the version 2 `cartridges/manifest.json`, downloads the
selected encrypted package on first play, verifies its length and SHA-256,
decrypts with WebCrypto, decompresses with a strict output-size bound, and
verifies the plaintext SHA-256 and catalog SHA-1. Decrypted bytes are cached
in the existing IndexedDB cartridge store. Repeat launches bypass decryption;
the gameplay worker, per-frame ROM reads and game cores are unchanged.

This is distribution obfuscation, not DRM or access control: the public manifest
deliberately supplies the decryption key and IV. A player can recover the bytes
from the browser. Existing downloaded copies cannot be revoked.

Saves stay in that browser, with import/export available. Localhost
saves do not migrate automatically to the production origin; export/import a
`.sav`. Use the primary site URL consistently because browser storage is scoped
to the origin.

Gzip sidecars and immutable hashed asset filenames reduce repeat downloads.
Encrypted packages are already compressed before encryption and do not get
gzip sidecars; their payload adds only the 16-byte GCM tag to the previous gzip size.
HTML and manifests revalidate. Nginx speaks h2c and Cloud Run uses `--use-http2`;
this is necessary for large cartridge responses given Cloud Run's
[HTTP/1 response limit](https://docs.cloud.google.com/run/quotas).
See Google's [HTTP/2 configuration](https://docs.cloud.google.com/run/docs/configuring/http2).

## Redeploy

Requires Node 22+, the repo's existing test toolchain and prepared cores, Docker
(OrbStack on this Mac), and a signed-in gcloud account with deployment access.
Existing GCP infrastructure is reused; the script does not provision projects or
grant project roles.

```sh
bash web/scripts/deploy-gcp.sh
```

The script runs tests/build, stages the explicit assets, builds linux/amd64,
pushes using a short-lived token in a temporary Docker config (removed on exit),
deploys publicly with bounded scaling, then validates both downloaded games.
`NP_GCP_PROJECT`, `NP_GCP_REGION`, `NP_GCP_TAG` override the defaults; the selected
project needs the matching Artifact Registry repository and runtime identity.
It builds locally, so no Cloud Build service-account role is needed.

## Verify an existing deployment

```sh
cd web
node --import tsx scripts/verify-deployment.ts https://nativeplat-1066182835702.us-east1.run.app/
```

This checks HTML/security headers, exactly the two selected games, complete ROM
fingerprints after decryption, old `.nds`/`.nds.gz` URLs returning 404, WASM
MIME/cache/digests, and 600 runtime frames per downloaded core.
It is a boot check, not full-game qualification. UI confirmation should also
exercise first play in a browser with no cached cartridge. The deployment does
not change game execution speed: gameplay still runs on the player's device.

`node --import tsx scripts/benchmark-packages.ts` measures local preparation
cost against gzip plus the original integrity checks (five samples, network
excluded). On this Mac with Node 22, medians were Diamond 344 ms → 349 ms and
Platinum 611 ms → 631 ms. These are first-load measurements, not browser FPS
measurements; browser/hardware timings will vary. Results are generated under
`build/gcp-package-benchmark.json`.
