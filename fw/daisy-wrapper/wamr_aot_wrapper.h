/**
 * wamr_aot_wrapper.h
 *
 * WAMR AOT chain wrapper for the DaisyOnline loader firmware.
 *
 * Supports loading 1–MAX_CHAIN_LEN AOT modules from a QSPI chain manifest and
 * running them as a serial audio processing chain.  Each module must export:
 *
 *   void process(const float* input, float* output, int num_samples)
 *
 * Optionally, modules may also export:
 *
 *   void _initialize()   (called once after instantiation; Emscripten convention)
 *
 * QSPI chain manifest format (written by the web tool at 0x90080000):
 *
 *   Bytes [0..3]              Magic:        0xDA157AC4
 *   Bytes [4..7]              module_count: uint32_t  (1..MAX_CHAIN_LEN)
 *   Bytes [8 .. 8+4*n-1]     size[i]:      uint32_t per-module AOT byte count
 *   Bytes [8+4*n .. end]      AOT binaries: concatenated, in chain order
 */

#pragma once
#include <wasm_export.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of modules in a chain. */
#define MAX_CHAIN_LEN 4

/* ── Print callback (routes wrapper diagnostics to hw.PrintLine) ────────────── */

typedef void (*wamr_print_callback_t)(const char* msg);
extern wamr_print_callback_t wamr_print_callback;
void wamr_print(const char* format, ...);

/* ── Chain handle ───────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t             module_count;
    wasm_module_t        modules     [MAX_CHAIN_LEN];
    wasm_module_inst_t   instances   [MAX_CHAIN_LEN];
    wasm_exec_env_t      exec_envs   [MAX_CHAIN_LEN];
    wasm_function_inst_t process_funcs[MAX_CHAIN_LEN];
    uint32_t             in_wasm_offs [MAX_CHAIN_LEN]; /* pre-allocated input  buffers */
    uint32_t             out_wasm_offs[MAX_CHAIN_LEN]; /* pre-allocated output buffers */
    uint32_t             audio_buf_samples;
} WamrAotChain;

/* ── Lifecycle ──────────────────────────────────────────────────────────────── */

/**
 * Allocate the chain struct and initialise the WAMR runtime.
 * The SDRAM allocator must be initialised before this call.
 * Returns NULL on failure.
 */
WamrAotChain* wamr_aot_chain_new(void);

/**
 * Destroy all modules in the chain and free all WAMR resources.
 */
void wamr_aot_chain_delete(WamrAotChain* chain);

/* ── Module loading ─────────────────────────────────────────────────────────── */

/**
 * Load and instantiate one AOT module, appending it to the chain.
 * Call once per module, in chain order, before wamr_aot_chain_finalize().
 *
 * @param chain      Chain created by wamr_aot_chain_new().
 * @param data       Pointer to the AOT binary (e.g. QSPI address).
 * @param size       Byte length of the AOT binary.
 * @param error_out  Optional buffer for a diagnostic string on failure.
 * @param error_len  Byte capacity of error_out.
 * Returns true on success.
 */
bool wamr_aot_chain_add_module(WamrAotChain*  chain,
                                const uint8_t* data,
                                uint32_t       size,
                                char*          error_out,
                                uint32_t       error_len);

/**
 * Pre-allocate per-module audio I/O buffers in WASM linear memory.
 * Must be called after all wamr_aot_chain_add_module() calls and before
 * wamr_aot_chain_process().
 *
 * @param chain               The populated chain.
 * @param audio_block_samples Number of samples per audio block (e.g. 48).
 * Returns true on success.
 */
bool wamr_aot_chain_finalize(WamrAotChain* chain, uint32_t audio_block_samples);

/* ── Audio processing ───────────────────────────────────────────────────────── */

/**
 * Run one audio block through the chain in series.
 * module[0] receives input; each module's output feeds the next; the final
 * module's output is written to output[].
 *
 * @param input       Host input buffer. May be NULL (treated as silence).
 * @param output      Host output buffer. Must be non-NULL.
 * @param num_samples Must be <= audio_block_samples set in wamr_aot_chain_finalize().
 */
void wamr_aot_chain_process(WamrAotChain* chain,
                             const float*  input,
                             float*        output,
                             int           num_samples);

#ifdef __cplusplus
}
#endif
