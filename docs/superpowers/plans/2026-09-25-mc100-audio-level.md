# MC100 V1 Audio Level Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make V1 boot recording short and adjustable, then add configurable DC blocking and fixed digital gain for listening tests.

**Architecture:** Product settings live in the recorder component. A portable PCM filter in `mc100_audio` processes a copied frame in the product capture callback before the existing queue. EVT capture, storage writer, and file formats remain unchanged.

**Tech Stack:** C11, ESP-IDF v6.1, ESP32-S3-MINI-1-N8, FreeRTOS, CMake/CTest, existing WAV/IDX verifier.

**Spec:** `docs/superpowers/specs/2026-09-25-mc100-audio-level-design.md`

## Global Constraints

- Do not alter or delete existing SD recordings or `.part` files.
- Use the locked ESP-IDF v6.1 and ESP32-S3 target for firmware builds.
- Keep the audio format at 16 kHz, signed 16-bit mono; preserve writer and EVT behavior.
- Compile-time settings: duration 3..600 seconds, DC block 0/1, gain 1..16; defaults 20 seconds, on, 8x.
- Do not claim acoustic acceptance based on build or WAV integrity alone.

---

### Task 1: Single Product Settings Header

**Files:**
- Create: `firmware/components/mc100_recorder/include/mc100_record_settings.h`
- Modify: `firmware/components/mc100_recorder/include/mc100_record_session.h`
- Modify: `firmware/tests/test_record_session.c`
- Modify: `firmware/main/Kconfig.projbuild`

**Interfaces:**
- Produces: `MC100_RECORD_DURATION_SECONDS`, `MC100_RECORD_DC_BLOCK_ENABLE`, and `MC100_RECORD_GAIN_X` macros; derived `MC100_RECORD_TARGET_FRAMES` and `MC100_RECORD_DEADLINE_SECONDS`.

- [x] Add a settings header with `#ifndef` defaults of 20, 1, and 8, followed by `#if` range checks that reject unsupported values at compile time.
- [x] Replace the two hard-coded session numbers with formulas: `target_frames = duration * 1000 / 20` and `deadline_seconds = duration + 10`.
- [x] Change the deadline test to assert against the derived constant and add a target-frame assertion, then run the focused host test and the default host suite.
- [x] Update the product Kconfig help so it no longer claims a fixed 600 seconds. Commit this independently testable settings change.

### Task 2: Portable PCM Processing

**Files:**
- Create: `firmware/components/mc100_audio/include/mc100_pcm_filter.h`
- Create: `firmware/components/mc100_audio/pcm_filter.c`
- Create: `firmware/tests/test_pcm_filter.c`
- Modify: `firmware/components/mc100_audio/CMakeLists.txt`
- Modify: `firmware/host/CMakeLists.txt`

**Interfaces:**
- Produces: `mc100_pcm_filter_t` holding Q16 DC estimate, input/output peak, and clipped-sample count; `mc100_pcm_filter_init(mc100_pcm_filter_t *)`; `mc100_pcm_filter_process(mc100_pcm_filter_t *, int16_t *, size_t, uint32_t gain, bool dc_block)` returning `mc100_result_t`.

- [x] Add a failing host test for identity (`dc_block=false`, gain 1), constant nonzero offset removal, an offset step `[0,1000]` that yields 998 at 1x, a low-level step `[0,1]` that yields 7 at 8x, gain, saturation boundaries, invalid arguments, and chunked versus whole-buffer continuity.
- [x] Add the filter source to both CMake graphs, run the focused test to confirm it fails, then implement the fixed-point filter using `sample_q16 = (int64_t)sample * 65536`, `dc_q16 += (sample_q16 - dc_q16) / 512`, and `(sample_q16 - dc_q16) * gain / 65536` before signed 16-bit clamp. Use 64-bit intermediates; validate arguments before mutating samples. Count clipping before clamp and record input peak before processing and output peak after clamp.
- [x] Run the focused test and full default host suite; inspect C11 warnings and commit the portable audio processor.

### Task 3: Product Integration and User Instructions

**Files:**
- Modify: `firmware/main/record_loop.c`
- Modify: `firmware/README.md`
- Modify: `README.md`
- Modify: `docs/MC100-SOFTWARE.md`
- Modify: `docs/MC100-VALIDATION.md`

**Interfaces:**
- Consumes: Task 1 settings and Task 2 `mc100_pcm_filter_process` API.
- Produces: product-only processed PCM; one boot configuration line and final input/output peak and clip-count metrics.

- [x] Initialize one filter state in the capture-owned runtime. In the frame callback, copy the 320-sample frame, call the processor with the settings macros, and enqueue the processed copy. On processing error, latch the existing capture fault path.
- [x] Add configuration to `RECORDER_BOOT` and filter metrics to `RECORDER_STOP`; only read capture metrics after producer quiescence. Keep existing queue and storage ownership unchanged.
- [x] Document the settings header, 20-second default, gain caveat, expected WAV/IDX output, and exact `-Profile product` build command. Update validation status without claiming a listening pass.
- [ ] Run host suite and locked target build. If COM7 and SD are available, run a 20-second capture, verify WAV/IDX, log peaks and clips, then commit. Otherwise record the physical test as pending and commit the verified software.

### Task 4: Independent Review

**Files:** none unless review finds a defect.

- [x] Review the complete diff for DSP arithmetic, state continuity, clipping, capture/queue ownership, config ranges, and product versus EVT isolation.
- [x] Fix any load-bearing findings, rerun focused/full host tests and target build, and record remaining physical validation limits.
