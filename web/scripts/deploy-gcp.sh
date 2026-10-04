#!/usr/bin/env bash
set -euo pipefail
# Run from any directory. Requires logged-in gcloud, Node 22+, and Docker.
np_root="$(cd "$(dirname "$0")/../.." && pwd)"
np_project="${NP_GCP_PROJECT:-nativeplat-20261004}"
np_region="${NP_GCP_REGION:-us-east1}"
np_tag="${NP_GCP_TAG:-$(date -u +%Y%m%d-%H%M%S)}"
np_registry="${np_region}-docker.pkg.dev"
np_image="${np_registry}/${np_project}/nativeplat/web:${np_tag}"
export CLOUDSDK_CORE_DISABLE_FILE_LOGGING=1
npm --prefix "$np_root/web" test
npm --prefix "$np_root/web" run build
node "$np_root/web/scripts/stage-gcp.mjs"
docker build --platform linux/amd64 -t "$np_image" "$np_root/build/gcp-nativeplat"
np_docker_host="${DOCKER_HOST:-$(docker context inspect --format '{{.Endpoints.docker.Host}}')}"
np_docker_auth="$(mktemp -d)"
trap 'rm -rf "$np_docker_auth"' EXIT
gcloud auth print-access-token | docker --config "$np_docker_auth" --host "$np_docker_host" login -u oauth2accesstoken --password-stdin "https://${np_registry}"
docker --config "$np_docker_auth" --host "$np_docker_host" push "$np_image"
gcloud run deploy nativeplat --project="$np_project" --region="$np_region" \
  --image="$np_image" --service-account="nativeplat-web@${np_project}.iam.gserviceaccount.com" \
  --allow-unauthenticated --ingress=all --use-http2 --port=8080 \
  --cpu=1 --memory=512Mi --concurrency=80 --min=0 --max=3 --timeout=300 --quiet
np_url="$(gcloud run services describe nativeplat --project="$np_project" --region="$np_region" --format='value(status.url)')"
cd "$np_root/web"
node --import tsx scripts/verify-deployment.ts "${np_url}/"
