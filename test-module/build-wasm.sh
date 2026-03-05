#!/bin/bash
set -e

WAMR_ROOT=../wasm-micro-runtime

echo "Building WASM module..."

# Clean old build artifacts
rm -f module.wasm module.aot module_aot.h

# Create build directory
mkdir -p build

# Check for emcc
if ! command -v emcc &> /dev/null; then
    echo "ERROR: emscripten not found!"
    echo "Please install with: brew install emscripten"
    exit 1
fi

echo "Using emscripten: $(which emcc)"

# Compile C++ to WASM using emscripten
# -I../src so that #include "../src/user.h" resolves correctly.
echo "Step 1: Compiling C++ to WASM..."
emcc \
    -O2 \
    -I../src \
    -sSTANDALONE_WASM \
    -sEXPORTED_RUNTIME_METHODS=[] \
    -sEXPORTED_FUNCTIONS=_process \
    -sERROR_ON_UNDEFINED_SYMBOLS=0 \
    --no-entry \
    -o build/module.wasm \
    module.cpp

echo "WASM module size: $(wc -c < build/module.wasm) bytes"

# Check for wamrc
if [ ! -f "$WAMR_ROOT/wamr-compiler/build/wamrc" ]; then
    echo ""
    echo "ERROR: wamrc not found at $WAMR_ROOT/wamr-compiler/build/wamrc"
    echo ""
    echo "Building wamrc AOT compiler (this takes a few minutes)..."
    pushd $WAMR_ROOT/wamr-compiler > /dev/null
    if [ ! -d "build" ]; then
        echo "Building LLVM dependencies..."
        ./build_llvm.sh
        mkdir -p build
        cd build
        echo "Building wamrc..."
        cmake ..
        make -j$(sysctl -n hw.ncpu)
    fi
    popd > /dev/null
    echo "wamrc build complete!"
fi

# Compile WASM to AOT for Cortex-M7
echo "Step 2: Compiling WASM to AOT..."
$WAMR_ROOT/wamr-compiler/build/wamrc \
    --target=thumbv7em \
    --cpu=cortex-m7 \
    --size-level=3 \
    --enable-builtin-intrinsics=i64.common,fp.common \
    -o build/module.aot \
    build/module.wasm

echo "AOT module size: $(wc -c < build/module.aot) bytes"

# Convert to C header using xxd
echo "Step 3: Embedding AOT in C header..."
xxd -i -n module_aot build/module.aot > build/module_aot.h

echo ""
echo "================================"
echo "Module build complete!"
echo "================================"
echo "Generated files:"
echo "  - build/module.wasm ($(wc -c < build/module.wasm) bytes)"
echo "  - build/module.aot ($(wc -c < build/module.aot) bytes)"
echo "  - build/module_aot.h (embedded)"
echo ""

echo "Flashing..."
./flash.sh build/module.aot
echo "Done!"
