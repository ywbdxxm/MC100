# MC100 V1 Audio Level Design

## Scope

The ESP32-S3-MINI-1-N8 product recorder continues to capture 16 kHz, signed
16-bit mono PCM from ZTS6872SE and save it through the existing queue and
writer. Add a small software DC blocker followed by fixed digital gain before
the product queue. Do not change the EVT capture path, WAV/IDX format, writer,
PDM slot, clock, or board wiring.

This is an adjustable listening experiment. It does not establish that the
microphone, acoustic path, or electrical interface meets a sensitivity target.
The previous WAV files remain the untouched baseline.

## User Settings

One product settings header defines:

- `MC100_RECORD_DURATION_SECONDS`: default 20, valid 3 through 600. This is
  the number of captured seconds, starting at the first accepted PCM frame.
- `MC100_RECORD_DC_BLOCK_ENABLE`: default 1, valid 0 or 1.
- `MC100_RECORD_GAIN_X`: default 8, valid 1 through 16. An 8x gain is about
  +18 dB. Gain 1 with DC blocking disabled reproduces the original PCM path.

The session frame target is duration times 50. Its safety deadline is duration
plus 10 seconds. The writer still rotates at 300 seconds; a 20-second session
normally publishes one WAV/IDX pair, and reserved `.part` files may remain.

## Processing

The capture task owns one stateful DC estimate across frames and writer
segments. Seed the estimate from the first sample. Update it as
`dc_q16 += (sample_q16 - dc_q16) / 512` and subtract it from each raw sample.
At 16 kHz this is approximately a 5 Hz DC-blocking corner, so ordinary music
and speech are preserved. Multiply the Q16 residual by the configured integer
gain before dividing once by 65536 (C signed division truncates toward zero),
then clamp to signed 16-bit range. Count samples whose integer amplified value
lies outside [-32768, 32767]. Input peak is the magnitude before processing;
output peak is the magnitude after clamping. Peak fields must represent 32768.
Use 64-bit intermediates throughout.

The product capture callback copies each frame, processes the copy, and
enqueues it. This preserves the existing assembler and writer contracts.
Configuration and statistics are printed once per session, not per frame.

## Verification

Host tests cover constant DC, a step after initialization, low-level samples
that reveal premature integer rounding, bypass identity, gain, clipping,
invalid gain, and continuity across frame boundaries. Build the product firmware with the
locked ESP-IDF v6.1/ESP32-S3 tuple. If target serial port and a card are available, capture
a short session and verify the final WAV/IDX with the existing verifier.
Compare processed audio with the unchanged baseline at the same source level,
distance, and microphone orientation. If the result is still too quiet or
noise becomes prominent, measure microphone VDD, PDM clock/data, and the
physical sound path before changing board design.
