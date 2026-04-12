#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build_linux"
PICO_SDK_PATH="${PICO_SDK_PATH:-/opt/pico-sdk}"

if [ ! -d "$PICO_SDK_PATH" ]; then
    echo "Error: Pico SDK not found at $PICO_SDK_PATH"
    echo "Set PICO_SDK_PATH to your pico-sdk location"
    exit 1
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [ ! -f "Makefile" ]; then
    PICO_SDK_PATH="$PICO_SDK_PATH" cmake "$SCRIPT_DIR"
fi

make -j"$(nproc)"

echo ""
echo "Build complete: ${BUILD_DIR}/k_line_kwp.uf2"
