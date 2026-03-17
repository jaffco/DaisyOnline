/**
 * DaisyOnline/fw/src/main.cpp
 *
 * WASM Loader Firmware for Daisy Seed
 *
 * Reads a WAMR AOT chain manifest from QSPI flash at QSPI_DATA_ADDR
 * (0x90080000) and runs the modules as a serial audio processing chain.
 *
 * Chain manifest format (written by the web tool):
 *
 *   Bytes [0..3]           Magic:        0xDA157AC4
 *   Bytes [4..7]           module_count: uint32_t  (1..MAX_CHAIN_LEN)
 *   Bytes [8..8+4*n-1]     size[i]:      uint32_t  per-module AOT byte count
 *   Bytes [8+4*n..end]     AOT binaries: concatenated, in chain order
 *
 * Each module must export:
 *   void process(const float* input, float* output, int num_samples)
 *
 * LED indicator (USER LED on the Daisy Seed):
 *   Solid on   = chain running, audio active
 *   Fast blink = Fatal error — check serial log
 *
 * Build
 * ─────
 *   cd fw && make
 *   cp fw/build/DaisyOnline-Loader.bin ../firmware/loader.bin
 */

#include "../libDaisy/src/daisy_seed.h"
#include "SDRAM.hpp"
#include "../daisy-wrapper/wamr_aot_wrapper.h"

using namespace daisy;

// ── Hardware & allocator ──────────────────────────────────────────────────────
static DaisySeed     hw;
static Jaffx::SDRAM  sdram;
static WamrAotChain* wamr_chain = nullptr;

// ── WAMR print callback ───────────────────────────────────────────────────────
static void WamrPrintHandler(const char* msg) { hw.PrintLine("%s", msg); }

// ── Memory layout ─────────────────────────────────────────────────────────────
// (written by the DaisyOnline browser tool)
//
//   0x90000000  QSPI base – Daisy bootloader reservation
//   0x90040000  This BOOT_SRAM firmware binary (written by "Flash Loader")
//   0x90080000  Chain manifest + AOT binaries (written by "Compile & Flash")
//
// Note: QSPI_BASE is already defined as a macro in stm32h750xx.h; use a
// project-local name to avoid the redefinition conflict.
static constexpr uint32_t DAISY_QSPI_BASE  = 0x90000000;
static constexpr uint32_t QSPI_DATA_OFFSET = 0x80000;           // 512 KB
static constexpr uint32_t QSPI_DATA_ADDR   = DAISY_QSPI_BASE + QSPI_DATA_OFFSET;

// Chain manifest constants
static constexpr uint32_t CHAIN_MAGIC         = 0xDA157AC4;  // "DAISY-CHAIN"
static constexpr uint32_t CHAIN_HDR_MIN_LEN   = 12;          // magic + count + 1 size
static constexpr uint32_t CHAIN_HDR_MAX_LEN   = 4 + 4 + MAX_CHAIN_LEN * 4; // magic + count + all sizes
static constexpr uint32_t CHAIN_MAX_TOTAL_AOT = 0x400000;    // 4 MB total AOT sanity cap

// ── SDRAM allocator C wrappers (required by WAMR) ────────────────────────────
// Jaffx::SDRAM's metadata struct is 24 bytes (a multiple of 8), so every
// buffer pointer inherits the 8-byte alignment of DAISY_SDRAM_BASE_ADDR
// (0xC0000000).  No alignment fixup is needed; these are plain wrappers.
extern "C" {
    void* sdram_alloc  (size_t size)             { return sdram.malloc(size); }
    void  sdram_dealloc(void* ptr)               { sdram.free(ptr); }
    void* sdram_realloc(void* ptr, size_t size)  { return sdram.realloc(ptr, size); }
    void* sdram_calloc (size_t nmemb, size_t size) { return sdram.calloc(nmemb, size); }
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
    if (!wamr_chain) {
        for (size_t i = 0; i < size; i++) out[0][i] = out[1][i] = 0.f;
        return;
    }

    // Run the chain (mono in/out; stereo via copy)
    const float* in_ptr = in ? in[0] : nullptr;
    wamr_aot_chain_process(wamr_chain, in_ptr, out[0], static_cast<int>(size));

    // Duplicate left channel to right
    for (size_t i = 0; i < size; i++) out[1][i] = out[0][i];
}

// ── Chain initialisation ──────────────────────────────────────────────────────
// Parses the chain manifest at `manifest`, loads each AOT module, and
// finalizes the chain with the given audio block size.
static bool InitChain(const uint8_t* manifest, uint32_t module_count,
                      const uint32_t* sizes)
{
    hw.PrintLine("Initialising WAMR runtime (%u module(s))...", module_count);

    wamr_chain = wamr_aot_chain_new();
    if (!wamr_chain) {
        hw.PrintLine("ERROR: wamr_aot_chain_new() returned NULL");
        return false;
    }

    // wasm_runtime_full_init() can disturb USB CDC interrupt priorities.
    // A short delay lets the USB stack drain before we try to print again.
    System::Delay(50);
    hw.PrintLine("WAMR runtime initialised — loading modules...");

    // AOT binaries start immediately after the manifest header.
    uint32_t header_len = 4 + 4 + module_count * 4; // magic + count + sizes[]
    const uint8_t* aot_ptr = manifest + header_len;

    char load_error[128] = {};
    for (uint32_t i = 0; i < module_count; i++) {
        hw.PrintLine("  module %u: ptr=0x%08X  size=%u bytes",
                     i, (uint32_t)(uintptr_t)aot_ptr, sizes[i]);

        load_error[0] = '\0';
        if (!wamr_aot_chain_add_module(wamr_chain, aot_ptr, sizes[i],
                                       load_error, sizeof(load_error))) {
            if (load_error[0])
                hw.PrintLine("DETAIL: %s", load_error);
            hw.PrintLine("ERROR: Failed to load module %u from QSPI", i);
            wamr_aot_chain_delete(wamr_chain);
            wamr_chain = nullptr;
            return false;
        }
        aot_ptr += sizes[i];
    }

    // NOTE: do NOT zero linear memory after instantiation.  The AOT loader
    // places initialised data segments (rodata, vtables, static initialisers)
    // into linear memory during wasm_runtime_instantiate.  Zeroing afterwards
    // would silently corrupt them.

    if (!wamr_aot_chain_finalize(wamr_chain, 48)) {
        hw.PrintLine("ERROR: wamr_aot_chain_finalize() failed");
        wamr_aot_chain_delete(wamr_chain);
        wamr_chain = nullptr;
        return false;
    }

    hw.PrintLine("Chain ready — %u module(s) loaded.", module_count);
    return true;
}

// ── Little-endian uint32 read helper ─────────────────────────────────────────
static inline uint32_t read_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

// ── main ─────────────────────────────────────────────────────────────────────
int main()
{
    hw.Init();
    hw.StartLog(); // non-blocking; use StartLog(true) to wait for terminal
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

    const uint8_t* p = reinterpret_cast<const uint8_t*>(QSPI_DATA_ADDR);

    // Invalidate D-cache over the maximum possible manifest header so we can
    // safely read magic, module_count, and all size[] fields before knowing
    // the total payload length.
    dsy_dma_invalidate_cache_for_buffer(
        const_cast<uint8_t*>(p), CHAIN_HDR_MAX_LEN);

    uint32_t magic        = read_u32_le(p);
    uint32_t module_count = read_u32_le(p + 4);

    hw.PrintLine("Header magic  : 0x%08X  (expected 0x%08X)", magic, CHAIN_MAGIC);
    hw.PrintLine("Module count  : %u", module_count);

    if (magic != CHAIN_MAGIC || module_count == 0 || module_count > MAX_CHAIN_LEN) {
        hw.PrintLine("ERROR: Invalid or missing chain manifest at 0x%08X", QSPI_DATA_ADDR);
        hw.PrintLine("       Flash a valid chain manifest, then reset.");
        ErrorLoop();
    }

    // Read per-module sizes and compute total AOT payload length.
    uint32_t sizes[MAX_CHAIN_LEN] = {};
    uint32_t total_aot = 0;
    for (uint32_t i = 0; i < module_count; i++) {
        sizes[i]   = read_u32_le(p + 8 + i * 4);
        total_aot += sizes[i];
        hw.PrintLine("  module %u size : %u bytes", i, sizes[i]);
    }

    if (total_aot == 0 || total_aot > CHAIN_MAX_TOTAL_AOT) {
        hw.PrintLine("ERROR: AOT payload size %u is out of range", total_aot);
        ErrorLoop();
    }

    // Invalidate D-cache for the full AOT payload region.
    uint32_t header_len = 4 + 4 + module_count * 4;
    dsy_dma_invalidate_cache_for_buffer(
        const_cast<uint8_t*>(p + header_len), total_aot);

    if (!InitChain(p, module_count, sizes)) {
        hw.PrintLine("FATAL: Chain initialisation failed — see log above.");
        ErrorLoop();
    }

    hw.PrintLine("Starting audio (48 kHz, 48-sample blocks)...");
    hw.StartAudio(AudioCallback);

    hw.PrintLine("Running.  LED solid = OK.");
    hw.SetLed(true);  // Solid on = success

    while (true) {}
}
