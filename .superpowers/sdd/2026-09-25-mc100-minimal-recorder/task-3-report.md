# Task 3 report: select the minimal MC100 product graph

## Scope

Task 3 changes the ESP-IDF component graph so the default `MC100_APP_PRODUCT`
image selects the minimal recorder path. `MC100_LEGACY_RUNTIME` defaults to
off and retains the old Supervisor/runtime path only when explicitly enabled.
The EVT profile keeps its diagnostic sources and legacy audio/core helpers.

## Files changed

- `firmware/main/Kconfig.projbuild`
  - Updated the product help text to describe the 600-second minimal recorder.
  - Added `MC100_LEGACY_RUNTIME`, default `n`, dependent on the product profile.
- `firmware/main/CMakeLists.txt`
  - Product source list: `app_main.c`, `record_loop.c`.
  - EVT source list: `app_main.c`, `evt_capture.c`, `evt_commands.c`.
  - Added `mc100_recorder` only to the product private requirements.
- `firmware/components/mc100_platform_espidf/CMakeLists.txt`
  - Default sources: `audio_i2s.c`, `sd_fat.c`, `board_io.c`.
  - `product_runtime.c` and the `mc100_supervisor` requirement are conditional
    on `MC100_LEGACY_RUNTIME`.
- `firmware/components/mc100_audio/CMakeLists.txt`
  - Default product source: `frame_assembler.c`.
  - `audio.c`, `preroll.c`, `stream.c`, and `vad_fixed.c` remain in EVT and
    legacy product profiles.
- `firmware/components/mc100_core/CMakeLists.txt`
  - Default product is header-only (`INCLUDE_DIRS "include"`).
  - `state.c`, `battery_policy.c`, and `upload_noop.c` remain in EVT and
    legacy product profiles.
- `firmware/components/mc100_recorder/CMakeLists.txt`
  - Reviewed and left unchanged: it already registers only portable
    `record_session.c`, requires `mc100_core`, and exposes `include`.

## Graph/source reasoning

The default product no longer reaches `product_runtime.c`, Supervisor, VAD,
pre-roll, battery policy, or upload code through target component requirements.
The target recorder component remains the only product-specific portable
session dependency. EVT still receives the existing capture and diagnostic
audio implementation. Explicitly enabling `MC100_LEGACY_RUNTIME` restores the
legacy platform/runtime and its core/audio dependencies for migration or bench
work.

## Commands and results

`git diff --check`

```text
(no output; passed)
```

`pwsh -File firmware/tools/build.ps1 -Profile product -Clean`

```text
Exception: firmware/tools/build.ps1:37
Activate the project's ESP-IDF environment first: missing or invalid IDF_PATH.
```

`pwsh -File firmware/tools/build.ps1 -Profile evt -Clean`

```text
Exception: firmware/tools/build.ps1:37
Activate the project's ESP-IDF environment first: missing or invalid IDF_PATH.
```

Environment inspection found `cmake`, `ninja`, and `idf.py` unavailable. No
tool installation was attempted. Consequently no target build or
`project_description.json` graph was generated in this task.

## Self-review

- Product and EVT branches have complete, mutually exclusive main source lists.
- Legacy runtime is opt-in and constrained to the product profile by Kconfig.
- Host CMake profile guards were not modified.
- The product graph intentionally references `record_loop.c`; Task 4 supplies
  that source, so a product build before Task 4 is expected to be incomplete.
- The recorder component already matched the requested portable-only contract,
  so it was not churned.

## Concerns

- Target profile builds remain unverified until the pinned ESP-IDF environment
  (`IDF_PATH`, `IDF_TOOLS_PATH`, `IDF_PYTHON_ENV_PATH`, CMake, and Ninja) is
  activated.
- The existing `app_main.c` still references the legacy product entry point;
  Task 4 must connect it to `record_loop.c` while preserving the explicit
  legacy switch.
