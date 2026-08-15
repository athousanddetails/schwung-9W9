#!/usr/bin/env bash
# Build the ER-99 module for Ableton Move (aarch64), or natively with NATIVE=1.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="schwung-er99-builder"
TARGET="${1:-all}"

if [ ! -f /.dockerenv ]; then
    echo "=== ER-99 build (Docker, ubuntu:22.04 / glibc 2.35) ==="
    docker image inspect "$IMAGE_NAME" >/dev/null 2>&1 || \
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
    docker run --rm -v "$REPO_ROOT:/build" -u "$(id -u):$(id -g)" -w /build \
        -e "NATIVE=${NATIVE:-0}" "$IMAGE_NAME" ./scripts/build.sh "$TARGET"
    exit 0
fi

cd "$REPO_ROOT"
if [ "${NATIVE:-0}" = "1" ]; then
    BUILD_DIR="build-native"
    echo "mode: NATIVE x86_64 (reference comparison only)"
    CC=gcc cmake -B "$BUILD_DIR" -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=Release
else
    BUILD_DIR="build"
    cmake -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
fi
cmake --build "$BUILD_DIR" --target "$TARGET" -j"$(nproc)"
# ---- Package for the Module Store ----
if [ "${NATIVE:-0}" != "1" ]; then
    rm -rf dist/9w9
    mkdir -p dist/9w9/samples
    cp "$BUILD_DIR/dsp.so"        dist/9w9/
    cp src/module.json            dist/9w9/
    cp src/ui_chain.js            dist/9w9/
    cp src/web_ui.html            dist/9w9/
    cp src/help.json              dist/9w9/
    cp src/movy_config.json       dist/9w9/
    cp src/samples/*.wav          dist/9w9/samples/
    (cd dist && tar -czf 9w9-module.tar.gz 9w9/)
    echo "Tarball: dist/9w9-module.tar.gz"
fi

echo; echo "=== Artifacts ==="
find "$BUILD_DIR" -maxdepth 1 -type f \( -name "*.so" -o -name "er99_render" \) -exec sh -c 'printf "%s\n  " "$1"; file -b "$1"' _ {} \;
