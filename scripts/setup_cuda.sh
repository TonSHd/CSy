#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$HERE/third_party/cuda"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "CSy: fetching real NVIDIA CUDA support (~600MB, one time)..."

pip download \
    nvidia-cuda-runtime-cu12 \
    nvidia-cublas-cu12 \
    --no-deps -d "$TMP"

mkdir -p "$DEST"
for whl in "$TMP"/*.whl; do
    echo "extracting $(basename "$whl")..."
    python3 -m zipfile -e "$whl" "$DEST"
done

echo "Done. nv:: (or cuda::) will pick this up automatically from now on."
