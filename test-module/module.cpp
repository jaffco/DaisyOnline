// test-module/module.cpp
//
// Local compilation test for DaisyOnline.
//
// Build with build-wasm.sh, then compare .wasm / .aot output against
// the web-compiled versions to isolate any toolchain differences.


// User code — defines void init() and float processSample(float input):
#include <cmath>
#define SAMPLE_RATE 48000

float processSample(float input) {
  static float phase = 0.f;
  phase += 220.f / SAMPLE_RATE; // SAMPLE_RATE macro comes pre-defined
  phase = phase > 1.f ? 0.f : phase;
  return std::sinf(phase * 2.f * M_PI);
}

// Buffer-processing wrapper — called by the WAMR runtime on Daisy hardware.
// Matches the wrapper the web IDE generates via CPP_TEMPLATE.
extern "C" {
  void process(const float* input, float* output, int num_samples) {
    for (int i = 0; i < num_samples; i++) {
      output[i] = processSample(input[i]);
    }
  }
}