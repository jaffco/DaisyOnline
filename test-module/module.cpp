// test-module/module.cpp
//
// Local compilation test for DaisyOnline.
// This file mirrors the CPP_TEMPLATE the web IDE builds in-browser:
//
//   1. Same macros (SAMPLE_RATE, M_PI) and headers.
//   2. #include "../src/user.h"  — the actual user code (init + processSample).
//   3. Identical process() wrapper exported as extern "C".
//
// Build with build-wasm.sh, then compare .wasm / .aot output against
// the web-compiled versions to isolate any toolchain differences.


// User code — defines void init() and float processSample():
// #include <cmath>
// #define SAMPLE_RATE 48000
// #include "../src/user.h"

float processSample() {
  static float phase = 0.f;
  phase += 0.001;
  if (phase >=  1.f) {phase = 0.f;}
  return phase;
}

// Buffer-processing wrapper — called by the WAMR runtime on Daisy hardware.
// Matches the wrapper the web IDE generates via CPP_TEMPLATE.
extern "C" {
  void __attribute__((used)) process(
    const float* input, float* output, int num_samples) {
    for (int i = 0; i < num_samples; i++) output[i] = processSample();
  }
}