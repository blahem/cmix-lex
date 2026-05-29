#!/usr/bin/env bash
set -euo pipefail

UPX_VERSION="${UPX_VERSION:-5.1.1}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLS_DIR="$ROOT_DIR/tools"
ARCHIVE="upx-${UPX_VERSION}-amd64_linux.tar.xz"
UPX_DIR="$TOOLS_DIR/upx-${UPX_VERSION}-amd64_linux"
URL="https://github.com/upx/upx/releases/download/v${UPX_VERSION}/${ARCHIVE}"

missing_tools=()
for tool in curl tar xz; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    missing_tools+=("$tool")
  fi
done
if (( ${#missing_tools[@]} )); then
  echo "Installing missing tools: ${missing_tools[*]}"
  sudo apt-get update
  sudo apt-get install curl tar xz-utils -y
fi

mkdir -p "$TOOLS_DIR"
if [[ ! -x "$UPX_DIR/upx" ]]; then
  curl -L --fail -o "$TOOLS_DIR/$ARCHIVE" "$URL"
  tar -C "$TOOLS_DIR" -xf "$TOOLS_DIR/$ARCHIVE"
fi

ln -sf "$UPX_DIR/upx" "$TOOLS_DIR/upx"
"$TOOLS_DIR/upx" --version
