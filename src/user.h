/*
 ____        _               ___        _ _
|  _ \  __ _(_)___ _   _   / _ \ _ __ | (_)_ __   ___
| | | |/ _` | / __| | | | | | | | '_ \| | | '_ \ / _ \
| |_| | (_| | \__ \ |_| | | |_| | | | | | | | | |  __/
|____/ \__,_|_|___/\__, |  \___/|_| |_|_|_|_| |_|\___|
                   |___/
*/

// ---------------------------------------------------------------------------
// DaisyOnline user.h
//
// Define two functions:
//   void  init()           - called once on startup (alloc, seed oscillators…)
//   float processSample()  - called once per output sample, return [-1, 1]
//
// DaisyOnline wraps these into:
//   void process(const float* input, float* output, int num_samples)
// which is used by BOTH the browser Web Audio engine (preview) AND the
// WAMR AOT runtime running on Daisy hardware (after flashing).
//
// Available macros: SAMPLE_RATE (44100), M_PI, and all of <cmath>/<iostream>.
// ---------------------------------------------------------------------------

void init() {
  // std::cout << "DaisyOnline init" << std::endl;
}

float processSample() {
  static float phase = 0.f;
  phase += 220.f / SAMPLE_RATE; // SAMPLE_RATE macro comes pre-defined
  phase = phase > 1.f ? 0.f : phase;
  return std::sinf(phase * 2.f * M_PI);
}