#!/usr/bin/env bash
set -euo pipefail
np_root="$(cd "$(dirname "$0")/../.." && pwd)"
np_env="$np_root/build/pokeweb-local.env"
mkdir -p "$np_root/build"
# Reuse this password across restarts; never bake it into the image or print it.
if [[ ! -f "$np_env" ]]; then
  (umask 077; python3 -c 'import secrets; print("NP_LOCAL_DB_PASSWORD=" + secrets.token_hex(32))' > "$np_env")
fi
VITE_CLOUD_SAVES=true npm --prefix "$np_root/web" run build
NP_WITH_SAVE_API=1 node "$np_root/web/scripts/stage-gcp.mjs"
docker compose --env-file "$np_env" -f "$np_root/web/deploy/compose.local.yml" up -d --build --wait
printf 'Local app: http://127.0.0.1:8088\nPostgres: 127.0.0.1:55438 / pokeweb\n'
