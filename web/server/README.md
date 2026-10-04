# Local account/save backend

No cloud database is provisioned. The existing public site is unchanged.

From the repository root, using Node 22+ and OrbStack/Docker:

```sh
bash web/scripts/local-up.sh
```

Open **http://127.0.0.1:8088**. Use Save manager to create an account or sign in.
Playing and local saves require no login. Each account has one server save total;
after connecting a slot, in-game saves upload automatically. Load the server save
on another browser/session to continue, or explicitly replace it with a local slot.
All account and sync UI stays inside Save manager.

The script builds the current frontend and stages only the existing allowlisted
cartridges/cores. It runs Postgres 16 and nginx/Node on a private Docker network,
with host ports bound only to loopback. Postgres is available at 127.0.0.1:55438,
database/user `pokeweb`. A generated password lives in ignored
`build/pokeweb-local.env` (mode 0600); do not commit or share it. The Docker volume
`pokeweb-local_saves` persists accounts and saves across container restarts.
Retain the env file while retaining that volume; changing it does not reset the
existing Postgres password.

Stop/start without deleting saved data:

```sh
docker compose --env-file build/pokeweb-local.env -f web/deploy/compose.local.yml stop
docker compose --env-file build/pokeweb-local.env -f web/deploy/compose.local.yml start
```

Do not use `down -v` unless intentionally deleting all local accounts/saves.
For SQL inspection, no password needs to be printed:

```sh
docker compose --env-file build/pokeweb-local.env -f web/deploy/compose.local.yml exec db psql -U pokeweb -d pokeweb
```

## Development and validation

`npm --prefix web test` covers local storage and the cloud sync client. Backend
integration tests require a **separate disposable database**, never the persistent
local app database. They create/delete fixture accounts and clear auth-limit rows:

```sh
docker run -d --name pokeweb-cloud-test -p 127.0.0.1:55439:5432 \
  -e POSTGRES_HOST_AUTH_METHOD=trust -e POSTGRES_DB=pokeweb_test postgres:16-alpine
npm --prefix web/server ci
TEST_DATABASE_URL=postgres://postgres@127.0.0.1:55439/pokeweb_test npm --prefix web/server test
```

For Vite, start the API with `DATABASE_URL`, `COOKIE_SECURE=false`, and
`APP_ORIGINS=http://127.0.0.1:5175`, then run Vite on port 5175. It proxies `/api`
to 127.0.0.1:8081. Cloud UI is on for development; production builds require
`VITE_CLOUD_SAVES=true`. `NP_WITH_SAVE_API=1` opts the staging script into the
combined image. The default GCP staging/deployment remains static.

## Future database deployment

The API accepts a standard `DATABASE_URL` or standard `PGHOST`, `PGPORT`,
`PGUSER`, `PGPASSWORD`, `PGDATABASE` variables. Production must use HTTPS,
secure cookies (the default), exact `APP_ORIGINS`, and the provider's verified
TLS connection settings. Credentials belong in the host's secret store.
No code depends on Cloud SQL. A future CockroachDB deployment still needs its
own compatibility run: PostgreSQL advisory migration locks and serializable
transaction retry behavior have not been adapted for CockroachDB.

Schema: accounts, hashed session tokens, one save keyed by account ID, and auth
throttle counters. Passwords use salted scrypt. Save validation checks supported
game, length, nonblank data, SHA-256, and optimistic revision; it does not certify
the cartridge's internal save checksums. Cross-device conflicts never overwrite
a newer save automatically. API failures leave IndexedDB data intact.

## Verified local result

The web suite has 24 passing tests; four real-Postgres integration tests cover
credentials/session lifecycle, account isolation, invalid uploads, one-save
replacement, concurrent writers/retries, and persistence across API restart.
The full local nginx/API/Postgres image also passed a real Platinum save round
trip: download, reach the saved field, quick save, upload, and byte-for-byte readback.
LibreWolf account creation and sign-out were checked in Save manager.

Repeat the packaged-app check from the repo root (fixture path is optional):

```sh
NP_TEST_SAVE=/absolute/path/to/platinum.sav node --import ./web/node_modules/tsx/dist/loader.mjs web/scripts/verify-cloud-local.ts
```

This check only targets loopback, reads the fixture without changing it, and
removes only its randomly named test accounts. Without a fixture it tests binary
storage integrity using generated data, without claiming game-save compatibility.
