/**
 * DaisyOnline/fw/src/main.cpp
 *
 * WASM Loader Firmware for Daisy Seed
 *
 * Reads a WAMR AOT module from QSPI flash at QSPI_DATA_ADDR (0x90080000).
 * The browser prepends an 8-byte header before the raw AOT binary:
 *
 *   Bytes [0..3]  Magic:  0xDA157A07  ("DAISy AOT")
 *   Bytes [4..7]  Size:   uint32_t, byte count of the AOT binary that follows
 *   Bytes [8..N]  The raw WAMR AOT binary
 *
 * The compiled WASM module must export:
 *   void process(const float* input, float* output, int num_samples)
 *
 * LED indicator (USER LED on the Daisy Seed):
 *   Solid on   = WAMR runtime running, audio active
 *   Fast blink = Fatal error — check serial log
 *
 * Build
 * ─────
 *   cd fw && make
 *   cp fw/build/DaisyOnline-Loader.bin ../firmware/loader.bin
 */

#include "../libDaisy/src/daisy_seed.h"
#include "SDRAM.hpp"

extern "C" {
#include "wasm_export.h"
}

#include "../daisy-wrapper/wamr_aot_wrapper.h"

using namespace daisy;

// ── Hardware & allocator ──────────────────────────────────────────────────────
static DaisySeed      hw;
static Jaffx::SDRAM   sdram;
static WamrAotEngine* wamr_engine = nullptr;

// ── WAMR print callback ───────────────────────────────────────────────────────
static void WamrPrintHandler(const char* msg) { hw.PrintLine("%s", msg); }

// ── Memory layout ─────────────────────────────────────────────────────────────
// (written by the DaisyOnline browser tool)
//
//   0x90000000  QSPI base – Daisy bootloader reservation
//   0x90040000  This BOOT_SRAM firmware binary (written by "Flash Loader")
//   0x90080000  User AOT binary + 8-byte header (written by "Compile & Flash")
//
// Note: QSPI_BASE is already defined as a macro in stm32h750xx.h; use a
// project-local name to avoid the redefinition conflict.
static constexpr uint32_t DAISY_QSPI_BASE  = 0x90000000;
static constexpr uint32_t QSPI_DATA_OFFSET = 0x80000;           // 512 KB
static constexpr uint32_t QSPI_DATA_ADDR   = DAISY_QSPI_BASE + QSPI_DATA_OFFSET;

// Header written by the browser before the AOT binary
static constexpr uint32_t AOT_MAGIC      = 0xDA157A07;
static constexpr uint32_t AOT_HEADER_LEN = 8;   // 4-byte magic + 4-byte size
static constexpr uint32_t AOT_MAX_SIZE   = 0x400000;  // 4 MB sanity cap

// ── SDRAM allocator C wrappers (required by WAMR) ────────────────────────────
// WAMR's EMS heap allocator enforces strict 8-byte alignment on every pool
// buffer it receives.  Jaffx::SDRAM's metadata struct is 20 bytes, which means
// raw returned pointers are always 4-byte aligned but never reliably 8-byte
// aligned.  These wrappers over-allocate by (sizeof(void*) + 7) bytes, nudge
// the returned address up to the next 8-byte boundary, and stash the original
// raw pointer one word before the aligned address so sdram_dealloc can recover
// it.  This pattern is identical to what wamr-demo uses.
extern "C" {
    void* sdram_alloc(size_t size) {
        void* raw = sdram.malloc(size + sizeof(void*) + 7);
        if (!raw) return nullptr;
        uintptr_t raw_addr     = (uintptr_t)raw + sizeof(void*);
        uintptr_t aligned_addr = (raw_addr + 7) & ~(uintptr_t)7;
        ((void**)aligned_addr)[-1] = raw;   // stash original for free
        return (void*)aligned_addr;
    }

    void sdram_dealloc(void* ptr) {
        if (!ptr) return;
        sdram.free(((void**)ptr)[-1]);
    }

    void* sdram_realloc(void* ptr, size_t size) {
        if (!ptr) return sdram_alloc(size);
        if (size == 0) { sdram_dealloc(ptr); return nullptr; }
        void* new_ptr = sdram_alloc(size);
        if (new_ptr) {
            memcpy(new_ptr, ptr, size);
            sdram_dealloc(ptr);
        }
        return new_ptr;
    }

    void* sdram_calloc(size_t nmemb, size_t size) {
        size_t total = nmemb * size;
        void* ptr = sdram_alloc(total);
        if (ptr) memset(ptr, 0, total);
        return ptr;
    }
}

// ── Error loop: fast LED blink ───────────────────────────────────────────────
[[ noreturn ]] static void ErrorLoop()
{
    bool led = false;
    while (true) {
        led = !led;
        hw.SetLed(led);
        System::Delay(100);
    }
}

// ── Audio callback ────────────────────────────────────────────────────────────
static void AudioCallback(AudioHandle::InputBuffer  in,
                          AudioHandle::OutputBuffer out,
                          size_t                    size)
{
    if (!wamr_engine) {
        for (size_t i = 0; i < size; i++) out[0][i] = out[1][i] = 0.f;
        return;
    }

    // Call the WASM process() function (mono in/out; stereo via copy)
    const float* in_ptr = in ? in[0] : nullptr;
    wamr_aot_engine_process(wamr_engine, in_ptr, out[0], static_cast<int>(size));

    // Duplicate left channel to right
    for (size_t i = 0; i < size; i++) out[1][i] = out[0][i];
}

// ── WAMR initialisation ───────────────────────────────────────────────────────
static bool InitWAMR(const uint8_t* aot_data, uint32_t aot_size)
{
    hw.PrintLine("Initialising WAMR runtime...");

    wamr_engine = wamr_aot_engine_new();
    if (!wamr_engine) {
        hw.PrintLine("ERROR: wamr_aot_engine_new() returned NULL");
        return false;
    }

    // wasm_runtime_full_init() can disturb USB CDC interrupt priorities.
    // A short delay lets the USB stack drain before we try to print again.
    System::Delay(50);
    hw.PrintLine("WAMR runtime initialised — loading module...");
    hw.PrintLine("  ptr=0x%08X  size=%u bytes", (uint32_t)(uintptr_t)aot_data, aot_size);

    char load_error[128] = {};
    if (!wamr_aot_engine_load_from_data(wamr_engine, aot_data, aot_size,
                                        load_error, sizeof(load_error))) {
        // load_error was filled by the wrapper — print it directly via hw.PrintLine
        // so it bypasses wamr_print (which may be unreliable after full_init).
        if (load_error[0])
            hw.PrintLine("DETAIL: %s", load_error);
        hw.PrintLine("ERROR: Failed to load AOT module from QSPI");
        wamr_aot_engine_delete(wamr_engine);
        wamr_engine = nullptr;
        return false;
    }

    // NOTE: do NOT zero linear memory here.  The AOT loader already places
    // initialised data segments (rodata, C++ vtables, static initialisers)
    // into linear memory during wasm_runtime_instantiate.  Zeroing afterwards
    // would silently corrupt those values and break any module that uses
    // virtual functions, std::string, or static-initialised objects.

    hw.PrintLine("WAMR ready — process() resolved.");
    return true;
}

// ── main ─────────────────────────────────────────────────────────────────────
int main()
{
    hw.Init();
    // hw.StartLog();   // non-blocking; use StartLog(true) to wait for terminal
    hw.StartLog(true);
    System::Delay(300);

    hw.PrintLine("==============================================");
    hw.PrintLine("  DaisyOnline – WASM Loader Firmware");
    hw.PrintLine("==============================================");
    hw.PrintLine("Data address : 0x%08X", QSPI_DATA_ADDR);

    // Route WAMR wrapper diagnostics through hw.PrintLine
    wamr_print_callback = WamrPrintHandler;

    // Initialise SDRAM (64 MB at 0xC0000000)
    sdram.init();
    hw.PrintLine("SDRAM initialised");

    // Invalidate D-cache over the 8-byte header
    dsy_dma_invalidate_cache_for_buffer(
        reinterpret_cast<uint8_t*>(QSPI_DATA_ADDR), AOT_HEADER_LEN);

    const uint8_t* p = reinterpret_cast<const uint8_t*>(QSPI_DATA_ADDR);

    // Little-endian reads
    uint32_t magic    = static_cast<uint32_t>(p[0])
                      | (static_cast<uint32_t>(p[1]) << 8)
                      | (static_cast<uint32_t>(p[2]) << 16)
                      | (static_cast<uint32_t>(p[3]) << 24);

    uint32_t aot_size = static_cast<uint32_t>(p[4])
                      | (static_cast<uint32_t>(p[5]) << 8)
                      | (static_cast<uint32_t>(p[6]) << 16)
                      | (static_cast<uint32_t>(p[7]) << 24);

    hw.PrintLine("Header magic : 0x%08X  (expected 0x%08X)", magic, AOT_MAGIC);
    hw.PrintLine("AOT size     : %u bytes", aot_size);

    if (magic != AOT_MAGIC || aot_size == 0 || aot_size > AOT_MAX_SIZE) {
        hw.PrintLine("ERROR: Invalid or missing AOT header at 0x%08X", QSPI_DATA_ADDR);
        hw.PrintLine("       Use DaisyOnline to compile & flash user code, then reset.");
        ErrorLoop();
    }

    // Invalidate D-cache for the full AOT binary region
    dsy_dma_invalidate_cache_for_buffer(
        const_cast<uint8_t*>(p + AOT_HEADER_LEN), aot_size);

    const uint8_t* aot_data = p + AOT_HEADER_LEN;

    if (!InitWAMR(aot_data, aot_size)) {
        hw.PrintLine("FATAL: WAMR initialisation failed — see log above.");
        ErrorLoop();
    }

    hw.PrintLine("Starting audio (48 kHz, 48-sample blocks)...");
    hw.StartAudio(AudioCallback);

    hw.PrintLine("Running.  LED solid = OK.");
    hw.SetLed(true);  // Solid on = success

    while (true) {}
}
