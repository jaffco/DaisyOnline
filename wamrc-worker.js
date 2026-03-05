/**
 * DaisyOnline/wamrc-worker.js  (type="module")
 *
 * Web Worker that lazily loads the pre-compiled wamrc Emscripten module from
 * WebWamrc's GitHub raw content, decompresses the Brotli-packed WASM blob,
 * and provides a single-call .wasm → .aot compilation service.
 *
 * Host → Worker messages
 * ──────────────────────
 *   { type: 'init' }
 *     Start lazy loading.  Worker replies with { type: 'ready' } when done.
 *
 *   { type: 'compile', id: <string>, wasmBytes: Uint8Array, target: <string> }
 *     Compile wasmBytes (.wasm) to AOT for the given target.
 *     Worker replies with:
 *       { type: 'result', id, aotBytes: Uint8Array }    on success
 *       { type: 'error',  id, message: <string> }       on failure
 *
 *   { type: 'status' }
 *     Worker replies with { type: 'status', ready: bool }
 *
 * wamrc target flags for Daisy Seed (Cortex-M7):
 *   --target=thumbv7em --cpu=cortex-m7
 */

const RELEASE_BASE   = 'https://jaffco.github.io/WebWamrc/prebuilt';
const WAMRC_JS_URL   = RELEASE_BASE + '/wamrc.js-2.4.3';
const WAMRC_WASM_URL = RELEASE_BASE + '/wamrc.js-2.4.wasm';

let wamrcModule   = null;   // Initialised Emscripten Module
let initialising  = false;
let initResolvers = [];     // pending onReady callbacks

// Captured output from the most recent wamrc invocation
let wamrcStdout = '';
let wamrcStderr = '';

// ── Logging ──────────────────────────────────────────────────────────────────

function log(...args)  { console.log('[wamrc-worker]', ...args); }
function warn(...args) { console.warn('[wamrc-worker]', ...args); }

// ── Initialisation ────────────────────────────────────────────────────────────

async function init() {
    if (wamrcModule)  return;
    if (initialising) return new Promise(r => initResolvers.push(r));
    initialising = true;

    log('Fetching wamrc JS glue from GitHub release...');
    const jsText = await fetch(WAMRC_JS_URL).then(r => {
        if (!r.ok) throw new Error('Failed to fetch wamrc.js: ' + r.status);
        return r.text();
    });

    // Wrap in a blob URL so we can dynamic-import() it as an ES module.
    const jsBlob = new Blob([jsText], { type: 'application/javascript' });
    const jsBlobUrl = URL.createObjectURL(jsBlob);

    log('Importing wamrc module factory...');
    // The JS glue may be a CommonJS IIFE or an ES module that exports a default.
    const factoryMod = await import(/* @vite-ignore */ jsBlobUrl);
    URL.revokeObjectURL(jsBlobUrl);

    const factory = factoryMod.default || factoryMod;
    if (typeof factory !== 'function') {
        throw new Error('wamrc.js-2.4.3 did not export a factory function');
    }

    log('Instantiating wamrc Emscripten module...');
    wamrcModule = await factory({
        // The JS glue is loaded from a blob URL, so _scriptDir is a blob URL.
        // Emscripten's findWasmBinary() would call new URL('wamrc.wasm', blobUrl)
        // which throws "Invalid URL".  locateFile() is called *instead of*
        // findWasmBinary() when provided, so we return the full CDN URL directly.
        // This mirrors how wasm-clang passes absolute URLs to compileStreaming().
        locateFile(path) {
            if (path.endsWith('.wasm')) return WAMRC_WASM_URL;
            return path;
        },
        noInitialRun:    true,
        noExitRuntime:   true,
        print(line)    { wamrcStdout += line + '\n'; },
        printErr(line) { wamrcStderr += line + '\n'; },
    });

    log('wamrc ready');

    initialising = false;
    for (const r of initResolvers) r();
    initResolvers = [];
}

// ── Compilation ───────────────────────────────────────────────────────────────

const INPUT_PATH  = '/tmp/input.wasm';
const OUTPUT_PATH = '/tmp/output.aot';

async function compile(wasmBytes, target) {
    if (!wamrcModule) throw new Error('wamrc is not yet initialised');

    const FS = wamrcModule.FS;

    try { FS.unlink(INPUT_PATH); }  catch (_) {}
    try { FS.unlink(OUTPUT_PATH); } catch (_) {}

    FS.writeFile(INPUT_PATH, wasmBytes instanceof Uint8Array ? wasmBytes : new Uint8Array(wasmBytes));

    // Build wamrc argv matching the WebWamrc demo exactly:
    // ["wamrc", `--target=${target}`, "-o", output, ...extraArgs, input]
    const args = [
        'wamrc',
        '--target=' + (target || 'thumbv7em'),
        '-o', OUTPUT_PATH,
        '--size-level=3',
        '--enable-builtin-intrinsics=i64.common,fp.common',
        '--cpu=cortex-m7',
        INPUT_PATH,
    ];
    // Reset captured output and re-wire print functions before each run.
    wamrcStdout = '';
    wamrcStderr = '';
    wamrcModule.print    = line => { wamrcStdout += line + '\n'; log('stdout:', line); };
    wamrcModule.printErr = line => { wamrcStderr += line + '\n'; warn('stderr:', line); };

    log('Invoking: ' + args.join(' '));

    // Allocate each argv string in WASM linear memory.
    const argc = args.length;
    const argPtrs = args.map(a => wamrcModule.allocateUTF8(a));
    // Build a C-style char** argv array in WASM memory.
    // _main(int argc, char **argv) requires argv to be a WASM pointer, not a JS array.
    // Allocate (argc + 1) * 4 bytes for the pointer array (null-terminated).
    const argv = wamrcModule._malloc((argc + 1) * 4);
    for (let i = 0; i < argc; i++) {
        wamrcModule.HEAP32[(argv >> 2) + i] = argPtrs[i];
    }
    wamrcModule.HEAP32[(argv >> 2) + argc] = 0; // null terminator

    let exitCode = 0;
    try {
        exitCode = wamrcModule._main(argc, argv);
    } catch (e) {
        // Some Emscripten builds throw process.exit() as a number.
        if (typeof e === 'number') exitCode = e;
        else throw e;
    } finally {
        // Free argv strings and the pointer array.
        for (const ptr of argPtrs) wamrcModule._free(ptr);
        wamrcModule._free(argv);
    }

    if (exitCode !== 0) {
        const detail = (wamrcStdout + wamrcStderr).trim();
        throw new Error('wamrc exited with code ' + exitCode +
            (detail ? ':\n' + detail : ''));
    }

    let aot;
    try {
        aot = FS.readFile(OUTPUT_PATH);
    } catch (e) {
        throw new Error('wamrc produced no output: ' + e.message);
    }
    log('AOT output: ' + aot.byteLength + ' bytes');
    return aot; // Uint8Array
}

// ── Message handler ───────────────────────────────────────────────────────────

self.onmessage = async function (ev) {
    const msg = ev.data;
    if (!msg || !msg.type) return;

    switch (msg.type) {
        case 'init':
            try {
                await init();
                self.postMessage({ type: 'ready' });
            } catch (err) {
                warn('init failed:', err);
                self.postMessage({ type: 'error', id: null, message: String(err) });
            }
            break;

        case 'status':
            self.postMessage({ type: 'status', ready: wamrcModule !== null });
            break;

        case 'compile': {
            const { id, wasmBytes, target } = msg;
            try {
                if (!wamrcModule) await init();
                const aotBytes = await compile(wasmBytes, target);
                self.postMessage({ type: 'result', id, aotBytes }, [aotBytes.buffer]);
            } catch (err) {
                warn('compile failed:', err);
                self.postMessage({ type: 'error', id, message: String(err) });
            }
            // Terminate so the host always gets a clean module on the next compile.
            self.close();
            break;
        }

        default:
            warn('Unknown message type:', msg.type);
    }
};
