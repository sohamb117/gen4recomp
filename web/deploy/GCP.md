# Public nativeplat deployment

Site: https://pokeweb.morisoba.moe/

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
npm run verify:deployment -- https://pokeweb.morisoba.moe/
```

The existing `run.app` address remains available. Browser saves are scoped to
the origin: export a `.sav` there and import it at the custom domain to continue
an existing save. The domain mapping does not alter the game binaries or pacing.

## Service

- Project: `nativeplat-20261004`; region: `us-east1`; service: `nativeplat`.
- Cloud Run serves nginx and the Node save API from Artifact Registry `nativeplat/web`.
- Runtime identity: `nativeplat-web@nativeplat-20261004.iam.gserviceaccount.com`,
  with no project-level roles. The site has public invocation access.
- One CPU, 512 MiB RAM, concurrency 80, zero minimum / three maximum instances.
- Billing uses the same account as the previously selected `monereko-20260809` project.
- Encrypted-package image digest:
  `sha256:41f6c976b76a2fa3cc23df9c1938ae752e252e3461dbea6fc4de942b7d06ce6d`.
- Initial encrypted-package revision: `nativeplat-00002-dl6` (historical).

The user explicitly selected public hosting of Diamond, Pearl, and Platinum. The
separate `scripts/stage-gcp.mjs` allowlists those exact local ROMs and verified
cores, checks their fingerprints, and writes only deployment assets into ignored
`build/gcp-nativeplat/`. Cartridges are gzip-compressed then AES-256-GCM encrypted
with fresh random keys and 96-bit IVs for each build, as hashed `.npc` files.
Raw `.nds` files are rejected by the staging audit and return 404 from nginx.
No saves, credentials, source checkout, diagnostic cores, or
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

Local saves stay in that browser, with import/export available. Optional accounts
can sync one save per account to Neon through Save manager. Localhost saves do not
migrate automatically to the production origin; export/import a `.sav`. Browser
storage remains scoped to the origin.

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
deploys publicly with bounded scaling, then validates all three downloaded games.
`NP_GCP_PROJECT`, `NP_GCP_REGION`, `NP_GCP_TAG` override the defaults; the selected
project needs the matching Artifact Registry repository and runtime identity.
It builds locally, so no Cloud Build service-account role is needed.

## Verify an existing deployment

```sh
cd web
npm run verify:deployment -- https://nativeplat-1066182835702.us-east1.run.app/
```

This checks HTML/security headers, exactly the three selected games, complete ROM
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

## Platinum recomp update — 2026-10-04

Historical revision: `nativeplat-00004-x52`.
Image: `us-east1-docker.pkg.dev/nativeplat-20261004/nativeplat/web@sha256:17ca3e7ec4ba5c7aa507c00cb89f7fcadf2f414da3988cc3c4aad3fc380eadac`.

Adds Preferences → Recomp options and Platinum quick save/F1. Diamond retains
its existing hosted core; Pearl and its diagnostic core are excluded while that
work is paused. No Platinum decomp patch was required.

Validation: 21 tests, production build, 12,000 Platinum runtime frames, all eight
resolution/aspect combinations, refused title-screen quick save, and successful
field quick save from a read-only local fixture. Post-deploy checks downloaded,
decrypted and verified both hosted cartridges and ran 600 frames with each
published core. The custom domain serves the new bundle over HTTPS with the
requested title and description.

## Local save backend — cloud provisioning cancelled

The optional account/save API is implemented and tested against local Postgres.
The user cancelled the paid Cloud SQL plan before any instance was created.
At that stage, no database, secret, IAM grant, or Cloud Run revision was provisioned
for this feature. SQL Admin and Secret Manager APIs were enabled during preparation;
these alone do not create database instances.

Cloud UI is enabled only for development or `VITE_CLOUD_SAVES=true` builds.
`NP_WITH_SAVE_API=1` opts into the combined nginx/Node image. The deployment
script now also accepts `NP_DATABASE_SECRET` and `NP_APP_ORIGINS`, migrates through
the direct Neon connection, and configures the existing service with pooled runtime
access. See [local and production setup](../server/README.md). Deployment requires
the database credential in Secret Manager and a secret-scoped accessor grant for
the runtime identity. Neon project `bold-sound-30444568`, branch `production`, is
linked locally; the original paid Cloud SQL plan remains cancelled.
`DATABASE_URL` supplies a Postgres connection without cloud-vendor-specific code.
CockroachDB has not been qualified;
its migrations and transaction retry behavior must be checked before switching.

## Neon cloud saves live — 2026-10-04

Historical revision `nativeplat-00005-sx4` introduced cloud saves on
https://pokeweb.morisoba.moe and the existing Google service URLs.
Image: `us-east1-docker.pkg.dev/nativeplat-20261004/nativeplat/web@sha256:907e2f47779d988b5b648d96075dd3a4ab787d93a38080823d97c043dd285bd1`.

The pooled Neon connection is injected from Secret Manager
`pokeweb-neon-database-url:1`, with certificate verification enabled. Only the
existing runtime identity was granted secret accessor on that secret; it gained
no project-level role. The schema was applied through the direct Neon connection
before deploying; startup migrations are disabled on Cloud Run.

Playing remains public. Save manager offers username/password accounts without
email verification. Sign in, then use **Save to cloud** on a local slot to connect
it. Subsequent in-game saves sync in the background. There is one cloud save total
per account; loading it creates a local slot and conflicts preserve local data.

Live validation passed: secure session cookies, registration, login/logout,
account isolation, invalid-upload rejection, stale-revision protection, replacing
a save while retaining one database row, and byte-for-byte Neon storage/readback.
Only temporary verification accounts were used and all were removed. Both hosted
cartridges and cores passed integrity checks and 600 runtime frames each.

## Merged main and Pearl live — 2026-10-04

Revision `nativeplat-00006-xbj` serves 100% of traffic at
https://pokeweb.morisoba.moe and the existing Google service URLs.
Cloud Run's resolved platform image:
`us-east1-docker.pkg.dev/nativeplat-20261004/nativeplat/web@sha256:1edd0a56cdbe1fbc9db56c42f83e5854ff7bd7bf7767cea4a1d2a29629130afe`.
Build tag: `20261004-main-a5fb536-three-games`.

All three hosted cores were rebuilt from isolated copies of merge `6de15fc30`
(main `352d36b23`) and optimized with the existing Asyncify/O3 recipe. The live
core manifest records their source commit and hashes and exactly matches the
qualified local manifest. No decomp/game logic patch was needed. Diamond and
Pearl now expose the shared camera, audio, text, bug-fix and quick-save controls.

Validation passed:

- 24 web tests and the production TypeScript/Vite build.
- 12,000 runtime frames per core; all eight resolution/aspect combinations and
  field quick-save persistence for Diamond, Pearl, and Platinum.
- Diamond and Pearl each replayed 12,600 new-game frames plus 3,400 Continue
  frames, producing a valid first save and a newer valid re-save for NATIVE.
- Both Google and custom-domain checks downloaded, decrypted and fingerprinted
  all three hosted cartridges, verified each core digest, and ran 600 frames
  per published core. Previously published plaintext URLs still return 404.
- The custom-domain account/save API passed secure sessions, login/logout,
  isolation, invalid-upload rejection, conflict protection and exact Neon byte
  readback. Replacing Platinum with Pearl and then Diamond retained one save
  row per account. Temporary verification accounts were removed.

Functional CLI checks use Node's baseline-only WASM testing mode to avoid Node
22 waiting on background TurboFan compilation after tests complete or during a
second instance load. The shipped modules and browser compiler behavior are
unchanged; these checks are not browser FPS measurements. The existing Neon
secret, runtime identity, optional-login behavior and service limits remain in use.
