/**
 * wamr_aot_wrapper.h
 *
 * Thin C wrapper around the WAMR AOT runtime for the DaisyOnline loader
 * firmware.  Modules are loaded from a caller-supplied pointer + length
 * (e.g. QSPI flash) rather than from an embedded header array.
 *
 * The loaded module must export:
 *   void process(const float* input, float* output, int num_samples)
 */

#pragma once
#include <wasm_export.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Print callback (routes wrapper diagnostics to hw.PrintLine) ────────────── */

typedef void (*wamr_print_callback_t)(const char* msg);
extern wamr_print_callback_t wamr_print_callback;
void wamr_print(const char* format, ...);

/* ── Engine handle ──────────────────────────────────────────────────────────── */

typedef struct {
    wasm_module_t        module;
    wasm_module_inst_t   instance;
    wasm_exec_env_t      exec_env;
    wasm_function_inst_t process_func;
} WamrAotEngine;

/* ── Lifecycle ──────────────────────────────────────────────────────────────── */

/**
 * Allocate and initialise the WAMR runtime engine.
 * Uses the SDRAM allocator (sdram_alloc / sdram_calloc / etc.) which must be
 * initialised before this call.
 * Returns NULL on failure.
 */
WamrAotEngine* wamr_aot_engine_new(void);

/**
 * Destroy the engine and free all WAMR resources.
 */
void wamr_aot_engine_delete(WamrAotEngine* engine);

/* ── Module loading ─────────────────────────────────────────────────────────── */

/**
 * Load and instantiate an AOT module from a raw byte buffer.
 * @param engine     Engine created by wamr_aot_engine_new().
 * @param data       Pointer to the AOT binary (e.g. QSPI address past the header).
 * @param size       Byte length of the AOT binary.
 * @param error_out  Optional caller-supplied buffer for a diagnostic string.
 *                   If non-NULL and the call fails, a human-readable reason is
 *                   written here so the caller can print it via hw.PrintLine()
 *                   (which is more reliable than wamr_print after
 *                   wasm_runtime_full_init() has been called).
 * @param error_len  Byte capacity of error_out.
 * Returns true on success.
 */
bool wamr_aot_engine_load_from_data(WamrAotEngine* engine,
                                    const uint8_t* data,
                                    uint32_t       size,
                                    char*          error_out,
                                    uint32_t       error_len);

/* ── Audio processing ───────────────────────────────────────────────────────── */

/**
 * Call the module's process(input, output, num_samples) export.
 * Input and output are host float arrays; the function copies them through
 * WASM linear memory.
 * @param input       May be NULL (passes a zero buffer to the module).
 * @param output      Must be non-NULL; filled with processed samples.
 * @param num_samples Number of samples in each buffer.
 */
void wamr_aot_engine_process(WamrAotEngine* engine,
                             const float*   input,
                             float*         output,
                             int            num_samples);

#ifdef __cplusplus
}
#endif
