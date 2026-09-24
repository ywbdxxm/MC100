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

## Fix round: ESP-IDF v6.1 conditional dependency ruling

Task 4's activated target build exposed an ESP-IDF v6.1 build-system rule:
`REQUIRES` and `PRIV_REQUIRES` must not depend on `CONFIG_*`; requirements are
expanded before configuration is loaded. The original Task 3 graph therefore
failed product compilation because `mc100_recorder` was conditionally hidden
from main, and the platform had the same latent issue for
`mc100_supervisor`.

The fix keeps main's source selection conditional but registers one
unconditional private requirement set that includes the portable
`mc100_recorder`. EVT builds compile the component because the dependency is
known to IDF, but do not reference its object code from the final image. This
is the smallest IDF-compliant deviation from the original profile-specific
requirement wording and does not add legacy code to EVT or product.

For the legacy Supervisor path, the platform component keeps its unconditional
core/storage/audio requirements and conditionally appends the existing
`product_runtime.c` plus `mc100_supervisor/supervisor.c` source files when
`MC100_LEGACY_RUNTIME=y`. Its private header path is supplied directly to the
platform target after registration. This avoids a CONFIG-dependent component
edge while leaving `mc100_supervisor` absent from the default product graph.

## Fix-round target verification

Activated with:

```powershell
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
```

Then ran:

```powershell
pwsh -File firmware/tools/build.ps1 -Profile product -Clean
pwsh -File firmware/tools/build.ps1 -Profile evt -Clean
```

Both completed with `Project build complete.` and
`MC100 target build verified; physical validation is recorded separately from
compilation.` `git diff --check` also passed. The build emitted only the
pre-existing FatFs Kconfig notes about boolean `default 0` values; no CMake
warnings or compiler errors remained.

Product `project_description.json` evidence (captured before the EVT rebuild):

```text
[main] app_main.c,record_loop.c
[mc100_audio] frame_assembler.c
[mc100_core] (header-only; no target sources)
[mc100_platform_espidf] audio_i2s.c,board_io.c,sd_fat.c
[mc100_recorder] record_session.c
[mc100_supervisor] ABSENT
```

EVT `project_description.json` evidence:

```text
[main] app_main.c,evt_capture.c,evt_commands.c
[mc100_audio] audio.c,frame_assembler.c,preroll.c,stream.c,vad_fixed.c
[mc100_core] battery_policy.c,state.c,upload_noop.c
[mc100_platform_espidf] audio_i2s.c,board_io.c,sd_fat.c
[mc100_recorder] record_session.c
[mc100_supervisor] ABSENT
```

The full command logs are retained as
`task-3-product-fix-build.log`, `task-3-evt-fix-build.log`, and
`task-3-legacy-fix-build.log` in this SDD directory. The explicit legacy build
was invoked with the pinned Python/IDF pair and temporary defaults containing
`CONFIG_MC100_APP_PRODUCT=y` and `CONFIG_MC100_LEGACY_RUNTIME=y`; it ended with
`Project build complete.` and had zero compiler errors.

## Fix-round self-review and concerns

- The product graph now builds with the exact pinned ESP-IDF v6.1 tuple and
  excludes `mc100_supervisor`, `product_runtime.c`, and legacy VAD sources.
- EVT still retains all diagnostic audio/core sources and has no Supervisor
  component edge.
- The portable recorder component is now present in EVT's component metadata
  due to IDF's unconditional requirement rule; it is tiny, has no legacy
  dependencies, and is not part of EVT's referenced source list.
- An explicit `MC100_LEGACY_RUNTIME=y` build also passed. Its graph includes
  `product_runtime.c` and `supervisor.c` under `mc100_platform_espidf` while
  the standalone `mc100_supervisor` component remains absent. IDF emits its
  expected validation warning that the reused `supervisor.c` belongs to that
  component; this is confined to the opt-in legacy profile and does not affect
  the default product/EVT builds.
