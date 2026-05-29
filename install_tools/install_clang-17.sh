#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLS_DIR="$ROOT_DIR/tools"
LLVM_SCRIPT="$TOOLS_DIR/llvm.sh"

mkdir -p "$TOOLS_DIR"
wget -O "$LLVM_SCRIPT" https://apt.llvm.org/llvm.sh
chmod +x "$LLVM_SCRIPT"
sudo "$LLVM_SCRIPT" 17
