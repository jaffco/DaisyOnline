# ──────────────────────────────────────────────────────────────────────────────
# DaisyOnline/fw/Makefile
#
# Builds the WASM loader SRAM firmware.
#
# After a successful build, copy the binary to the web application:
#   cp build/DaisyOnline-Loader.bin ../firmware/loader.bin
# ──────────────────────────────────────────────────────────────────────────────

TARGET = DaisyOnline-Loader

CPP_SOURCES = src/main.cpp
C_SOURCES   = daisy-wrapper/wamr_aot_wrapper.c

# Include WAMR before common.mk so that C_SOURCES and C_INCLUDES are populated
include wamr.mk

include common.mk
