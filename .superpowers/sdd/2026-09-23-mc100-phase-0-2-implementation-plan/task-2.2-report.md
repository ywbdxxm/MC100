# Task 2.2 implementation report

## Scope

Implemented the per-frame VAD decision seam and deterministic fixed-sequence
placeholder. `mc100_vad_t` carries a `decide` callback plus opaque context;
`mc100_vad_fixed()` fires once when a frame sequence equals `trigger_seq`.
No real VAD behavior or Phase 1 spike code was changed.

## Changes

- Added `firmware/components/mc100_audio/include/mc100_vad.h`.
- Added `firmware/components/mc100_audio/vad_fixed.c`.
- Added and registered `firmware/tests/test_vad_fixed.c`.
- Registered `vad_fixed.c` in both the ESP-IDF audio component and host audio
  library; registered the host `vad_fixed` test with strict C11 checks.

## TDD evidence

The test was written and registered before the production header/source. The
required host command was attempted during RED and again after implementation:

`pwsh -File firmware/tools/test-host.ps1`

Both attempts stopped before configure because this environment has no `cmake`
executable (the script reports “Missing cmake”). No host-suite pass can be
claimed until CMake/Ninja/CTest and a C11 compiler are available. `git diff
--check` passes.

## API and registration invariants

The requested callback/context layout and fixed-state fields are used verbatim.
The audio component continues to require `mc100_core`, which supplies
`mc100_types.h`; host and ESP-IDF source registrations remain aligned.

## Concerns

- Host RED/GREEN execution is environment-blocked by missing build tools.
- Existing unrelated untracked `firmware/spikes/` content was preserved and is
  not included in this task's commit.
