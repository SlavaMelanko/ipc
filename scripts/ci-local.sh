#!/usr/bin/env bash
# Runs .github/workflows/ci-ubuntu.yml locally in Docker, so CI failures can
# be reproduced without pushing. Requires Docker.
set -euo pipefail

cd "$(dirname "$0")/.."

readonly IMAGE="ubuntu:26.04"

docker run --rm -v "$(pwd)":/host-repo:ro "$IMAGE" bash -c '
set -euo pipefail

apt-get update -qq
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
  cmake gcc-15 g++-15 clang-format clang-tidy clangd >/dev/null

mkdir -p /workspace
cd /host-repo
tar -cf - --exclude=build --exclude=.git . | (cd /workspace && tar -xf -)
cd /workspace

echo "=== Setup ==="
CC=gcc-15 CXX=g++-15 cmake -S . -B build

echo "=== Format check ==="
cmake --build build --target format-check

echo "=== Lint ==="
cmake --build build --target tidy

echo "=== Build ==="
cmake --build build

echo "=== Test ==="
ctest --test-dir build --output-on-failure
'
