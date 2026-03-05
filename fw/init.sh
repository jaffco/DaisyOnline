#!/usr/bin/env bash
# DaisyOnline/fw/init.sh
#
# Initialises the firmware build environment:
#   1. Clones / updates the libDaisy and wasm-micro-runtime git submodules
#   2. Builds libDaisy (required by the linker)
#
# Prerequisites: ARM GCC toolchain (arm-none-eabi-gcc) and cmake in $PATH.
# See https://daisy.audio/tutorials/cpp-dev-env/#1-install-the-toolchain

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[1;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
ORANGE='\033[38;5;208m'
BOLD='\033[1m'
NC='\033[0m'

echo -e "${ORANGE}Initialising DaisyOnline firmware environment...${NC}"

# ── Submodules ────────────────────────────────────────────────────────────────
echo -e "${BLUE}Fetching submodules (libDaisy + wasm-micro-runtime)...${NC}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# .gitmodules lives at the repo root (one level above fw/)
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Submodule paths are relative to the repo root
git -C "$REPO_ROOT" submodule update --recursive --init -- fw/libDaisy fw/wasm-micro-runtime

# ── Build libDaisy ────────────────────────────────────────────────────────────
LIBDAISY_DIR="$SCRIPT_DIR/libDaisy"

if [ ! -f "$LIBDAISY_DIR/src/daisy_seed.h" ]; then
    echo -e "${RED}ERROR: libDaisy not found at $LIBDAISY_DIR${NC}"
    echo -e "${YELLOW}Check that the git submodule initialised correctly.${NC}"
    exit 1
fi

echo -e "${BLUE}Building libDaisy...${NC}"
cd "$LIBDAISY_DIR"
make -s clean
make -j"$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)" -s

if [ $? -ne 0 ]; then
    echo -e "${RED}Failed to build libDaisy.${NC}"
    echo -e "${YELLOW}Have you installed the Daisy Toolchain?${NC}"
    echo -e "${YELLOW}See: https://daisy.audio/tutorials/cpp-dev-env/#1-install-the-toolchain${NC}"
    exit 1
fi

echo -e "${GREEN}libDaisy built successfully.${NC}"
echo ""
echo -e "${GREEN}Init complete!${NC}"
echo -e "${YELLOW}Next: ${CYAN}${BOLD}cd fw && make${NC}${YELLOW} to build the firmware.${NC}"
echo -e "${YELLOW}Then: ${CYAN}${BOLD}cp fw/build/DaisyOnline-Loader.bin ../firmware/loader.bin${NC}"
