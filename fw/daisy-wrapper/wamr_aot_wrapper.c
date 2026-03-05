/**
 * wamr_aot_wrapper.c
 *
 * Implementation of the DaisyOnline WAMR AOT wrapper.
 *
 * Unlike the wamr-demo wrapper, modules are loaded from a caller-supplied
 * pointer (e.g. QSPI flash) rather than from an embedded header array.
 * Everything else – SDRAM allocator, WASM linear memory allocation for
 * cross-boundary buffer passing – is identical to the wamr-demo approach.
 */

#include "wamr_aot_wrapper.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* Stack / heap sizes for the WASM instance */
#define STACK_SIZE  8192
#define HEAP_SIZE   (16 * 1024)

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

WamrAotEngine* wamr_aot_engine_new(void)
{
    WamrAotEngine* engine = (WamrAotEngine*)sdram_calloc(1, sizeof(WamrAotEngine));
    if (!engine) return NULL;

    RuntimeInitArgs init_args = {0};
    init_args.mem_alloc_type = Alloc_With_Allocator;
    init_args.mem_alloc_option.allocator.malloc_func  = (void*)_wamr_calloc_wrapper;
    init_args.mem_alloc_option.allocator.realloc_func = (void*)sdram_realloc;
    init_args.mem_alloc_option.allocator.free_func    = (void*)sdram_dealloc;

    if (!wasm_runtime_full_init(&init_args)) {
        sdram_dealloc(engine);
        return NULL;
    }

    return engine;
}

void wamr_aot_engine_delete(WamrAotEngine* engine)
{
    if (!engine) return;
    if (engine->exec_env)  wasm_runtime_destroy_exec_env(engine->exec_env);
    if (engine->instance)  wasm_runtime_deinstantiate(engine->instance);
    if (engine->module)    wasm_runtime_unload(engine->module);
    wasm_runtime_destroy();
    sdram_dealloc(engine);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Module loading                                                              */
/* ═══════════════════════════════════════════════════════════════════════════ */

bool wamr_aot_engine_load_from_data(WamrAotEngine* engine,
                                    const uint8_t* data,
                                    uint32_t       size,
                                    char*          error_out,
                                    uint32_t       error_len)
{
    char error_buf[128];

    /* Write msg to error_out (for hw.PrintLine in caller) and wamr_print. */
#define _REPORT(msg) \
    do { \
        if (error_out && error_len > 0) \
            snprintf(error_out, error_len, "%s", msg); \
        wamr_print("%s\n", msg); \
    } while(0)

    wamr_print("Loading AOT module: ptr=%p  size=%u bytes\n", (void*)data, size);

    /* wasm_runtime_load() treats its first argument as modifiable; the cast to
       uint8_t* is safe because the runtime only inspects it during this call. */
    engine->module = wasm_runtime_load(
        (uint8_t*)(uintptr_t)data, size, error_buf, sizeof(error_buf));

    if (!engine->module) {
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "wasm_runtime_load failed: %s  [first 4 bytes: %02x %02x %02x %02x]",
                 error_buf, data[0], data[1], data[2], data[3]);
        _REPORT(detail);
        return false;
    }

    engine->instance = wasm_runtime_instantiate(
        engine->module, STACK_SIZE, HEAP_SIZE, error_buf, sizeof(error_buf));

    if (!engine->instance) {
        char detail[128];
        snprintf(detail, sizeof(detail), "wasm_runtime_instantiate failed: %s", error_buf);
        _REPORT(detail);
        return false;
    }

    engine->exec_env = wasm_runtime_create_exec_env(engine->instance, STACK_SIZE);
    if (!engine->exec_env) {
        _REPORT("wasm_runtime_create_exec_env failed");
        return false;
    }

    engine->process_func = wasm_runtime_lookup_function(engine->instance, "process");
    if (!engine->process_func) {
        _REPORT("'process' export not found - export: void process(float*, float*, int)");
        return false;
    }

#undef _REPORT
    wamr_print("AOT module loaded and instantiated. process() resolved.\n");
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Audio processing                                                            */
/* ═══════════════════════════════════════════════════════════════════════════ */

void wamr_aot_engine_process(WamrAotEngine* engine,
                             const float*   input,
                             float*         output,
                             int            num_samples)
{
    if (!engine || !engine->process_func) return;

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

    /* Allocate buffers inside WASM linear memory */
    size_t   nbytes    = (size_t)num_samples * sizeof(float);
    uint32_t in_off    = wasm_runtime_module_malloc(engine->instance, nbytes, NULL);
    uint32_t out_off   = wasm_runtime_module_malloc(engine->instance, nbytes, NULL);

    if (!in_off || !out_off) {
        if (in_off)  wasm_runtime_module_free(engine->instance, in_off);
        if (out_off) wasm_runtime_module_free(engine->instance, out_off);
        /* Emit silence on failure */
        memset(output, 0, nbytes);
        return;
    }

    /* Copy host → WASM */
    void* in_native = wasm_runtime_addr_app_to_native(engine->instance, in_off);
    if (in_native) {
        if (input)
            memcpy(in_native, input, nbytes);
        else
            memset(in_native, 0, nbytes);
    }

    /* Call process(input_ptr, output_ptr, num_samples) */
    uint32_t argv[3] = { in_off, out_off, (uint32_t)num_samples };

    if (wasm_runtime_call_wasm(engine->exec_env,
                               engine->process_func, 3, argv))
    {
        /* Copy WASM → host */
        void* out_native = wasm_runtime_addr_app_to_native(engine->instance, out_off);
        if (out_native)
            memcpy(output, out_native, nbytes);
    }
    else
    {
        static int err_count = 0;
        if (err_count < 3) {
            const char* exc = wasm_runtime_get_exception(engine->instance);
            wamr_print("ERROR: process() call failed — %s\n", exc ? exc : "(no exception)");
            ++err_count;
        }
        memset(output, 0, nbytes);
    }

    wasm_runtime_module_free(engine->instance, in_off);
    wasm_runtime_module_free(engine->instance, out_off);
}
