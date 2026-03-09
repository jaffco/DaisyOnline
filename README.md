<div align="center">

  # DaisyOnline: Write Daisy DSP on the Web!

  [![Try Live Demo](https://img.shields.io/badge/🚀_Try_Live_Demo-blue?style=for-the-badge&logoColor=white)](https://jaffco.github.io/DaisyOnline/)

</div>

---

DaisyOnline is a browser-based integrated development environment (IDE) for writing digital signal processing (DSP) programs for the [Daisy hardware platform](https://daisy.audio/). 

It enables real-time audio synthesis and effects development directly in your web browser, and browser-based deployment to Daisy hardware.

## Features

- **Browser-based C++ DSP development** - Write audio processing code using familiar C++ syntax
- **Real-time audio preview** - Audition your DSP algorithms instantly using Web Audio API
- **Client-side compilation** - Powered by [wasm-clang](https://github.com/binji/wasm-clang) and [wamrc](https://github.com/bytecodealliance/wasm-micro-runtime) — no server-side build required
- **One-click hardware deployment** - Flash compiled DSP modules directly to Daisy devices via WebUSB
- **Monaco Editor** - Professional code editing with syntax highlighting, IntelliSense-like features, and bracket matching
- **LocalStorage persistence** - Your code is saved in your browser and persists between sessions
- **WebAssembly Micro Runtime (WAMR)** - Runs your DSP code efficiently on Daisy's ARM Cortex-M7 processor

## How It Works

1. **Write C++ DSP Code** - Implement `init()` and `processSample()` functions in the browser editor
2. **Preview in Browser** - Compile to WebAssembly and preview audio using Web Audio API
3. **Deploy to Hardware** - Compile to AOT binary and flash to Daisy via USB for standalone operation

## Getting Started

### 🌐 Try Online (Recommended)
**[Visit https://jaffco.github.io/DaisyOnline](https://jaffco.github.io/DaisyOnline)** - No setup required! Just open your browser and start coding C++ DSP.

### 🛠️ Run Locally

1. Clone this repository
2. Start the development server: `npm start`
3. Navigate to `http://localhost:3000/` in your browser

> **Note:** All compilation happens entirely in the browser. The first build may take a moment to download the compiler toolchain (~50 MB).

## Project Structure

- `src/` - Default DSP code template and user.h header
- `fw/` - Daisy firmware build system (WAMR loader)
- `test-module/` - Example DSP module with build/flash scripts
- `firmware/` - Pre-built firmware binaries for flashing
- `dfu/` - WebDFU libraries for USB flashing to Daisy
- `wasm-micro-runtime/` - WAMR submodule for AOT compilation

## Building Firmware

The Daisy firmware uses a custom WAMR (WebAssembly Micro Runtime) engine to execute your compiled DSP code. The static site provides pre-built firmware binaries in the `firmware/` directory, but you can also build from source:

```bash
cd fw
make clean && make
make program-dfu  # Flash loader firmware to Daisy
# The firmware runs from SRAM, 
# so must be flashed to a Daisy with a bootloader installed.
```

## Flashing DSP Modules

After writing your DSP code in the browser:

1. Click "Build & Run" to compile and preview
2. Click "Flash to Daisy" to compile to AOT and deploy to hardware

## Requirements

- Modern web browser with WebAssembly and WebUSB support
- Daisy hardware with bootloader installed
- USB connection for flashing

## Contributing

DaisyOnline is open source! Contributions are welcome:

- Report bugs or request features via GitHub Issues
- Submit pull requests for improvements
- Help improve documentation

## License

MIT License - see [LICENSE](LICENSE) file for details.

## Acknowledgments

- Built on [wasm-clang](https://github.com/binji/wasm-clang) for client-side C++ compilation
- Uses [WebAssembly Micro Runtime (WAMR)](https://github.com/bytecodealliance/wasm-micro-runtime) for hardware execution
- Powered by [Monaco Editor](https://microsoft.github.io/monaco-editor/) for the coding experience
- DFU flashing via [WebDFU](https://github.com/devanlai/webdfu)