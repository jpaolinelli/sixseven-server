#!/usr/bin/env bash
set -euo pipefail

IMAGE="${IMAGE:-undiscoveredtech/sixseven_db_server}"
TAG="${TAG:-latest}"
PLATFORM="${PLATFORM:-}"
PUSH="${PUSH:-false}"

FULL_IMAGE="${IMAGE}:${TAG}"

echo "Building: ${FULL_IMAGE}"

BUILD_ARGS=(
    --target runtime
    -t "${FULL_IMAGE}"
)

if [ -n "${PLATFORM}" ]; then
    BUILD_ARGS+=(--platform "${PLATFORM}")
fi

if [ "${PUSH}" = "true" ]; then
    BUILD_ARGS+=(--push)
else
    BUILD_ARGS+=(--load)
fi

docker buildx build "${BUILD_ARGS[@]}" .

echo ""
echo "Done: ${FULL_IMAGE}"
if [ "${PUSH}" = "true" ]; then
    echo "Pushed to registry."
else
    echo ""
    echo "Run:  docker run -p 6767:6767 ${FULL_IMAGE}"
    echo "Push: PUSH=true TAG=${TAG} ./scripts/docker-build.sh"
fi
