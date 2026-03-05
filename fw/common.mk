# ──────────────────────────────────────────────────────────────────────────────
# DaisyOnline/fw/common.mk – shared build settings for the loader firmware
#
# libDaisy location is resolved in order:
#   1. LIBDAISY_DIR environment variable (override: make LIBDAISY_DIR=/path …)
#   2. A local ./libDaisy submodule (populated by ./init.sh)
# ──────────────────────────────────────────────────────────────────────────────

CONFIG_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

ifneq ($(LIBDAISY_DIR),)
    # Use explicit override — nothing to do here
else ifneq ($(wildcard $(CONFIG_DIR)libDaisy/src/daisy_seed.h),)
    LIBDAISY_DIR := $(CONFIG_DIR)libDaisy
    $(info Using local libDaisy: $(LIBDAISY_DIR))
else
    $(error libDaisy not found. Run ./init.sh or set LIBDAISY_DIR=/path/to/libDaisy)
endif

LIBDAISY_DIR := $(subst \,/,$(LIBDAISY_DIR))
$(if $(wildcard $(LIBDAISY_DIR)),,$(error libDaisy not found at $(LIBDAISY_DIR)))

# ── Build configuration ───────────────────────────────────────────────────────

SYSTEM_FILES_DIR := $(LIBDAISY_DIR)/core

# APP_TYPE = BOOT_SRAM: the Daisy bootloader copies this binary from QSPI to
# SRAM before executing it, leaving QSPI free for AOT module data.
APP_TYPE = BOOT_SRAM

# Use the 10 ms bootloader timeout for a faster development loop.
BOOT_BIN = $(SYSTEM_FILES_DIR)/dsy_bootloader_v6_3-intdfu-10ms.bin

CPP_STANDARD = -std=gnu++14
OPT          = -Ofast

# daisy-wrapper is already on the include path via wamr.mk; the libDaisy core
# Makefile picks up C_INCLUDES and C_SOURCES automatically.
include $(SYSTEM_FILES_DIR)/Makefile
