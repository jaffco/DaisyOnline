/**
 * wamr_aot_wrapper.c
 *
 * Implementation of the DaisyOnline WAMR AOT chain wrapper.
 *
 * A single WAMR runtime hosts up to MAX_CHAIN_LEN module instances.  Each
 * module is loaded from a caller-supplied pointer (e.g. a region of QSPI
 * flash), instantiated with its own stack/heap, and chained serially in the
 * audio ISR via wamr_aot_chain_process().
 */

#include "wamr_aot_wrapper.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* Per-instance stack and heap sizes. */
#define STACK_SIZE  8192
#define HEAP_SIZE   (16 * 1024)

/* Upper bound on audio block size used for intermediate SRAM buffers.
   Must be >= the audio_block_samples passed to wamr_aot_chain_finalize(). */
#define MAX_AUDIO_BLOCK_SAMPLES 128

/* ── Print callback ───────────────────────────────────────────────────────── */

wamr_print_callback_t wamr_print_callback = NULL;

void wamr_print(const char* format, ...) {
    if (!wamr_print_callback) return;
    char buf[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    wamr_print_callback(buf);
}

/* ── External SDRAM allocator (defined in main.cpp) ───────────────────────── */
extern void* sdram_alloc  (size_t size);
extern void* sdram_realloc(void* ptr, size_t size);
extern void  sdram_dealloc(void* ptr);
extern void* sdram_calloc (size_t nmemb, size_t size);

/* WAMR requires a malloc-style function (single argument); wrap calloc so that
   all WAMR allocations are zero-initialised (prevents subtle AOT runtime bugs). */
static void* _wamr_calloc_wrapper(unsigned size)
{
    return sdram_calloc(1, (size_t)size);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Lifecycle                                                                   */
/* ═══════════════════════════════════════════════════════════════════════════ */

WamrAotChain* wamr_aot_chain_new(void)
{
    WamrAotChain* chain = (WamrAotChain*)sdram_calloc(1, sizeof(WamrAotChain));
    if (!chain) return NULL;

    RuntimeInitArgs init_args = {0};
    init_args.mem_alloc_type = Alloc_With_Allocator;
    init_args.mem_alloc_option.allocator.malloc_func  = (void*)_wamr_calloc_wrapper;
    init_args.mem_alloc_option.allocator.realloc_func = (void*)sdram_realloc;
    init_args.mem_alloc_option.allocator.free_func    = (void*)sdram_dealloc;

    if (!wasm_runtime_full_init(&init_args)) {
        sdram_dealloc(chain);
        return NULL;
    }

    return chain;
}

void wamr_aot_chain_delete(WamrAotChain* chain)
{
    if (!chain) return;

    for (uint32_t i = 0; i < chain->module_count; i++) {
        if (chain->instances[i]) {
            if (chain->in_wasm_offs[i])
                wasm_runtime_module_free(chain->instances[i], chain->in_wasm_offs[i]);
            if (chain->out_wasm_offs[i])
                wasm_runtime_module_free(chain->instances[i], chain->out_wasm_offs[i]);
        }
        if (chain->exec_envs[i])   wasm_runtime_destroy_exec_env(chain->exec_envs[i]);
        if (chain->instances[i])   wasm_runtime_deinstantiate(chain->instances[i]);
        if (chain->modules[i])     wasm_runtime_unload(chain->modules[i]);
    }

    wasm_runtime_destroy();
    sdram_dealloc(chain);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Module loading                                                              */
/* ═══════════════════════════════════════════════════════════════════════════ */

bool wamr_aot_chain_add_module(WamrAotChain*  chain,
                                const uint8_t* data,
                                uint32_t       size,
                                char*          error_out,
                                uint32_t       error_len)
{
    if (!chain) return false;

    if (chain->module_count >= MAX_CHAIN_LEN) {
        wamr_print("ERROR: chain is full (%d modules max)\n", MAX_CHAIN_LEN);
        if (error_out && error_len > 0)
            snprintf(error_out, error_len, "chain is full (%d modules max)", MAX_CHAIN_LEN);
        return false;
    }

    uint32_t i = chain->module_count;
    char error_buf[128];

#define _REPORT(msg) \
    do { \
        if (error_out && error_len > 0) \
            snprintf(error_out, error_len, "[module %u] %s", i, msg); \
        wamr_print("[module %u] %s\n", i, msg); \
    } while(0)

    wamr_print("Loading module %u: ptr=%p  size=%u bytes\n", i, (void*)data, size);

    chain->modules[i] = wasm_runtime_load(
        (uint8_t*)(uintptr_t)data, size, error_buf, sizeof(error_buf));

    if (!chain->modules[i]) {
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "wasm_runtime_load failed: %s  [first 4 bytes: %02x %02x %02x %02x]",
                 error_buf, data[0], data[1], data[2], data[3]);
        _REPORT(detail);
        return false;
    }

    chain->instances[i] = wasm_runtime_instantiate(
        chain->modules[i], STACK_SIZE, HEAP_SIZE, error_buf, sizeof(error_buf));

    if (!chain->instances[i]) {
        char detail[128];
        snprintf(detail, sizeof(detail), "wasm_runtime_instantiate failed: %s", error_buf);
        _REPORT(detail);
        return false;
    }

    chain->exec_envs[i] = wasm_runtime_create_exec_env(chain->instances[i], STACK_SIZE);
    if (!chain->exec_envs[i]) {
        _REPORT("wasm_runtime_create_exec_env failed");
        return false;
    }

    chain->process_funcs[i] = wasm_runtime_lookup_function(chain->instances[i], "process");
    if (!chain->process_funcs[i]) {
        _REPORT("'process' export not found — export: void process(float*, float*, int)");
        return false;
    }

    /* Call _initialize() if exported (Emscripten static constructors). */
    wasm_function_inst_t init_func =
        wasm_runtime_lookup_function(chain->instances[i], "_initialize");
    if (init_func) {
        if (!wasm_runtime_call_wasm(chain->exec_envs[i], init_func, 0, NULL)) {
            const char* exc = wasm_runtime_get_exception(chain->instances[i]);
            wamr_print("[module %u] WARNING: _initialize() failed — %s\n",
                       i, exc ? exc : "(no exception)");
            /* Non-fatal: module may still process correctly. */
        }
    }

#undef _REPORT

    chain->module_count++;
    wamr_print("Module %u loaded, instantiated, process() resolved.\n", i);
    return true;
}

bool wamr_aot_chain_finalize(WamrAotChain* chain, uint32_t audio_block_samples)
{
    if (!chain || chain->module_count == 0) return false;

    size_t nbytes = audio_block_samples * sizeof(float);

    for (uint32_t i = 0; i < chain->module_count; i++) {
        chain->in_wasm_offs[i]  = wasm_runtime_module_malloc(chain->instances[i], nbytes, NULL);
        chain->out_wasm_offs[i] = wasm_runtime_module_malloc(chain->instances[i], nbytes, NULL);

        if (!chain->in_wasm_offs[i] || !chain->out_wasm_offs[i]) {
            wamr_print("ERROR: failed to pre-allocate WASM audio buffers for module %u\n", i);
            return false;
        }
    }

    chain->audio_buf_samples = audio_block_samples;
    wamr_print("Chain finalized: %u module(s), %u samples/block.\n",
               chain->module_count, audio_block_samples);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Audio processing                                                            */
/* ═══════════════════════════════════════════════════════════════════════════ */

void wamr_aot_chain_process(WamrAotChain* chain,
                             const float*  input,
                             float*        output,
                             int           num_samples)
{
    if (!chain || chain->module_count == 0) return;

    size_t nbytes = (size_t)num_samples * sizeof(float);

    /* Guard: block size must not exceed the pre-allocated buffer capacity. */
    if ((uint32_t)num_samples > chain->audio_buf_samples) {
        memset(output, 0, nbytes);
        return;
    }

    /* Initialise WAMR thread environment for the calling thread (audio ISR).
       Safe to call multiple times; no-ops after first success. */
    static __thread bool thread_env_init = false;
    if (!thread_env_init) {
        if (!wasm_runtime_init_thread_env()) {
            wamr_print("ERROR: wasm_runtime_init_thread_env failed\n");
            return;
        }
        thread_env_init = true;
    }

    /* Intermediate host-side buffers declared static so they live in SRAM
       (BSS), keeping inter-module copies off the FMC/SDRAM bus. */
    static float intermediate[MAX_CHAIN_LEN - 1][MAX_AUDIO_BLOCK_SAMPLES];

    const float* cur_in = input;

    for (uint32_t i = 0; i < chain->module_count; i++) {

        float* cur_out = (i == chain->module_count - 1) ? output : intermediate[i];

        /* Copy cur_in → module's WASM input buffer. */
        void* in_native = wasm_runtime_addr_app_to_native(
            chain->instances[i], chain->in_wasm_offs[i]);
        if (in_native) {
            if (cur_in)
                memcpy(in_native, cur_in, nbytes);
            else
                memset(in_native, 0, nbytes);
        }

        /* Call process(input_ptr, output_ptr, num_samples). */
        uint32_t argv[3] = {
            chain->in_wasm_offs[i],
            chain->out_wasm_offs[i],
            (uint32_t)num_samples
        };

        if (wasm_runtime_call_wasm(chain->exec_envs[i],
                                   chain->process_funcs[i], 3, argv))
        {
            /* Copy module's WASM output → cur_out. */
            void* out_native = wasm_runtime_addr_app_to_native(
                chain->instances[i], chain->out_wasm_offs[i]);
            if (out_native)
                memcpy(cur_out, out_native, nbytes);
        }
        else
        {
            static int err_count = 0;
            if (err_count < 3) {
                const char* exc = wasm_runtime_get_exception(chain->instances[i]);
                wamr_print("ERROR: module %u process() failed — %s\n",
                           i, exc ? exc : "(no exception)");
                ++err_count;
            }
            memset(cur_out, 0, nbytes);
        }

        cur_in = cur_out;
    }
}
