#!/usr/bin/env bash
# One-shot developer setup: install tooling, expose it on PATH, configure the
# build (which also enables the pre-commit hook). Safe to re-run.
set -euo pipefail

cd "$(dirname "$0")/.."

link_brew_tool() {
  # Homebrew's llvm is keg-only, so its binaries are not on PATH by default.
  # Symlink the ones we need without shadowing the system Apple Clang compiler.
  local name=$1
  local src="/opt/homebrew/opt/llvm/bin/${name}"
  local dst="/opt/homebrew/bin/${name}"
  if [[ -x "$src" && ! -e "$dst" ]]; then
    ln -s "$src" "$dst"
    echo "linked ${name}"
  fi
}

cmake_env=()

case "$(uname)" in
  Darwin)
    if ! command -v brew >/dev/null; then
      echo "Homebrew is required: https://brew.sh" >&2
      exit 1
    fi
    echo "installing tooling via Homebrew..."
    brew install cmake clang-format llvm
    link_brew_tool clang-tidy
    link_brew_tool clangd
    ;;
  Linux)
    if ! command -v apt-get >/dev/null; then
      echo "Automated setup supports apt-based distros only." >&2
      echo "Install: build-essential, cmake, gcc-15, g++-15, clang-format, clang-tidy, clangd, zlib1g-dev." >&2
      exit 1
    fi
    echo "installing tooling via apt..."
    sudo apt-get update
    sudo apt-get install -y build-essential cmake gcc-15 g++-15 clang-format \
      clang-tidy clangd zlib1g-dev
    # Default apt gcc/g++ may be older than 15; pin explicitly.
    cmake_env=(CC=gcc-15 CXX=g++-15)
    ;;
  *)
    echo "Unsupported platform: $(uname)" >&2
    exit 1
    ;;
esac

echo "configuring build (enables the pre-commit hook)..."
env "${cmake_env[@]+"${cmake_env[@]}"}" cmake -S . -B build

echo "done. build with: cmake --build build"
