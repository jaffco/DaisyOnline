# ──────────────────────────────────────────────────────────────────────────────
# DaisyOnline/fw/wamr.mk
#
# WAMR runtime build configuration for Daisy Seed (Cortex-M7, AOT-only mode).
# Mirrors wamr-demo/wamr.mk — adapted path variables for this firmware tree.
# ──────────────────────────────────────────────────────────────────────────────

WAMR_ROOT_DIR := $(shell pwd)/wasm-micro-runtime

# ── Feature flags ─────────────────────────────────────────────────────────────
WAMR_BUILD_PLATFORM      = daisy
WAMR_BUILD_TARGET        = THUMBV7EM
WAMR_BUILD_INTERP        = 0
WAMR_BUILD_FAST_INTERP   = 0
WAMR_BUILD_AOT           = 1
WAMR_BUILD_JIT           = 0
WAMR_BUILD_FAST_JIT      = 0
WAMR_BUILD_LIBC_BUILTIN  = 1
WAMR_BUILD_LIBC_WASI     = 0
WAMR_BUILD_SIMD          = 0
WAMR_BUILD_MULTI_MODULE  = 0
WAMR_BUILD_SHARED_MEMORY = 0
WAMR_BUILD_MINI_LOADER   = 1

# ── Directory shortcuts ───────────────────────────────────────────────────────
WAMR_CORE_DIR   := $(WAMR_ROOT_DIR)/core/iwasm
WAMR_SHARED_DIR := $(WAMR_ROOT_DIR)/core/shared

# ── Core runtime sources ──────────────────────────────────────────────────────
C_SOURCES += \
    $(WAMR_CORE_DIR)/common/wasm_runtime_common.c \
    $(WAMR_CORE_DIR)/common/wasm_native.c \
    $(WAMR_CORE_DIR)/common/wasm_memory.c \
    $(WAMR_CORE_DIR)/common/wasm_exec_env.c \
    $(WAMR_CORE_DIR)/common/wasm_c_api.c \
    $(WAMR_CORE_DIR)/common/wasm_loader_common.c

# ── AOT runtime sources ───────────────────────────────────────────────────────
C_SOURCES += \
    $(WAMR_CORE_DIR)/aot/aot_loader.c \
    $(WAMR_CORE_DIR)/aot/aot_runtime.c \
    $(WAMR_CORE_DIR)/aot/arch/aot_reloc_thumb.c \
    $(WAMR_CORE_DIR)/aot/aot_intrinsic.c

# ── Memory allocator ──────────────────────────────────────────────────────────
C_SOURCES += \
    $(WAMR_SHARED_DIR)/mem-alloc/ems/ems_kfc.c \
    $(WAMR_SHARED_DIR)/mem-alloc/ems/ems_alloc.c \
    $(WAMR_SHARED_DIR)/mem-alloc/ems/ems_hmu.c \
    $(WAMR_SHARED_DIR)/mem-alloc/ems/ems_gc.c \
    $(WAMR_SHARED_DIR)/mem-alloc/mem_alloc.c

# ── Platform abstraction layer (custom Daisy platform) ────────────────────────
WAMR_PLATFORM_DIR := $(WAMR_SHARED_DIR)/platform/daisy
C_SOURCES += \
    $(WAMR_PLATFORM_DIR)/daisy_platform.c \
    $(WAMR_PLATFORM_DIR)/daisy_thread.c \
    $(WAMR_PLATFORM_DIR)/daisy_time.c \
    $(WAMR_SHARED_DIR)/platform/common/math/math.c

# ── libc-builtin ──────────────────────────────────────────────────────────────
C_SOURCES += \
    $(WAMR_CORE_DIR)/libraries/libc-builtin/libc_builtin_wrapper.c

# ── invokeNative trampoline for Thumb2 + VFP (Cortex-M7 with FPU) ────────────
ASM_SOURCES += \
    $(WAMR_CORE_DIR)/common/arch/invokeNative_thumb_vfp.s

# ── Utility sources ───────────────────────────────────────────────────────────
C_SOURCES += \
    $(WAMR_SHARED_DIR)/utils/bh_assert.c \
    $(WAMR_SHARED_DIR)/utils/bh_bitmap.c \
    $(WAMR_SHARED_DIR)/utils/bh_common.c \
    $(WAMR_SHARED_DIR)/utils/bh_hashmap.c \
    $(WAMR_SHARED_DIR)/utils/bh_list.c \
    $(WAMR_SHARED_DIR)/utils/bh_log.c \
    $(WAMR_SHARED_DIR)/utils/bh_queue.c \
    $(WAMR_SHARED_DIR)/utils/bh_vector.c \
    $(WAMR_SHARED_DIR)/utils/runtime_timer.c

# ── Include paths ─────────────────────────────────────────────────────────────
C_INCLUDES += \
    -I$(WAMR_CORE_DIR)/include \
    -I$(WAMR_CORE_DIR)/common \
    -I$(WAMR_CORE_DIR)/aot \
    -I$(WAMR_SHARED_DIR)/include \
    -I. \
    -I$(WAMR_SHARED_DIR)/platform/include \
    -I$(WAMR_PLATFORM_DIR) \
    -I$(WAMR_SHARED_DIR)/mem-alloc \
    -I$(WAMR_SHARED_DIR)/utils \
    -Idaisy-wrapper

# ── Preprocessor defines ──────────────────────────────────────────────────────
C_DEFS += \
    -DCOMPILING_WASM_RUNTIME_API=1 \
    -DBUILD_TARGET_THUMB_V7EM \
    -DBUILD_TARGET=\"THUMBV7EM\" \
    -DWASM_ENABLE_AOT=1 \
    -DWASM_ENABLE_INTERP=0 \
    -DWASM_ENABLE_FAST_INTERP=0 \
    -DWASM_ENABLE_JIT=0 \
    -DWASM_ENABLE_FAST_JIT=0 \
    -DWASM_ENABLE_LIBC_BUILTIN=1 \
    -DWASM_ENABLE_LIBC_WASI=0 \
    -DWASM_ENABLE_BULK_MEMORY=1 \
    -DWASM_ENABLE_MULTI_MODULE=0 \
    -DWASM_ENABLE_SHARED_MEMORY=0 \
    -DWASM_ENABLE_MINI_LOADER=1 \
    -DWASM_DISABLE_HW_BOUND_CHECK=1 \
    -DWASM_DISABLE_STACK_HW_BOUND_CHECK=1 \
    -DBH_PLATFORM_DAISY

# ── Suppress noisy WAMR warnings ──────────────────────────────────────────────
CFLAGS += -Wno-unused-parameter -Wno-unused-variable
