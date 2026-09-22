# MC100 firmware USB recording bench

This is a manual `EVT_USB_BENCH` recording slice using the portable audio and
storage components. It supports microphone capture, two-second prerecord,
bounded buffering, WAV/index writing and five-minute rotation. It does not yet
provide automatic LISTEN/VAD, startup recovery or product battery safety.
The bench explicitly requires USB power with no battery; it never automatically
starts recording. See the [actual test report](../docs/reports/2026-09-19-evt-recording.md).

## Identity and environment checks

The board contract is MC100 V1 / ESP32-S3-MINI-1-N8: target `esp32s3`, 8 MB Flash,
no PSRAM. `dependencies.lock.json` pins the exact ESP-IDF v6.1 revision and records
that no managed third-party components are used yet. Do not replace the SDK with
a different ambient installation or install tools into an unidentified environment.

For target work, activate the project's matching ESP-IDF installation in a fresh
PowerShell terminal. For EIM, select its matching installation-specific activation
profile; generic `export.ps1` is not a substitute. Be aware that some generated EIM
profiles update the global selection: keep that side effect disabled when only
performing a project build. `build.ps1` does not activate or install any environment.

Before building it verifies the clean SDK checkout/revision, active IDF-owned Python,
tools root, target compiler, CMake and Ninja. Every native command must exit zero.
After building it verifies the generated SDK/target/config metadata, Flash size,
80 MHz startup CPU frequency, USB Serial/JTAG primary console, brownout and watchdogs,
excluded wireless/PSRAM components, and decoded partition table.

## Host tests

Use an activated native C11 compiler and CMake/Ninja/CTest environment (on Windows,
a Visual Studio Developer PowerShell with MSVC C11 support is suitable). Do not use
the ESP32 cross-compiler as the host compiler. From the repository root:

On Windows this project has two separate tool tuples. Host tests require the MSVC
Developer environment (`vcvars64.bat`) so that `cl.exe`, the Windows SDK and standard
headers resolve correctly. The ESP-IDF profile must not replace that environment; when
both are needed, prepend only the validated IDF CMake/Ninja/Python paths. Run from
`cmd.exe`/PowerShell, not Git Bash, because `MSYSTEM`/`MINGW_*` causes ESP-IDF activation
to reject the shell.

```powershell
pwsh -File firmware/tools/test-host.ps1 -Clean
pwsh -File firmware/tests/test_build_tools.ps1
```

The equivalent portable CMake commands are:

```powershell
cmake -S firmware/host -B firmware/out/host -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build firmware/out/host
ctest --test-dir firmware/out/host --output-on-failure
```

Tests use production public headers, standard C assertions (NDEBUG is explicitly
undefined), C11, and compiler warnings as errors. On a GCC/Clang host, pass
`-Sanitizers` to the script (or `-DMC100_ENABLE_SANITIZERS=ON` to CMake) for ASan/UBSan.
MSVC sanitizer requests fail rather than silently skip UBSan.

## Target build

From the same activated ESP-IDF terminal:

```powershell
pwsh -File firmware/tools/build.ps1 -Profile evt -Clean
```

The target tuple is ESP-IDF v6.1 at the revision recorded in
`dependencies.lock.json`, with its matching IDF-owned Python, tools root, CMake, Ninja
and ESP32-S3 compiler. Activate the installation-specific v6.1 PowerShell profile
before running the script. A generic `esp-idf/export.ps1` is not sufficient on this
machine when it resolves Python from uv; do not install or mix another SDK. Do not run
bare `idf.py build`: it bypasses the output-directory and SDK/configuration guardrails
in `firmware/tools/build.ps1`.

The script invokes the validated installation's Python and `idf.py` with absolute
project, output, `SDKCONFIG`, defaults and target arguments. Outputs and generated
configuration stay under `firmware/out/target`; the user's `firmware/sdkconfig` and
existing `firmware/build` remain untouched. `-OutputDirectory out/<new-name>` supports
a separate output. Traversal outside `firmware/out`, output-root deletion and linked
output ancestors are rejected. `-Clean` deletes only the checked generated directory.
Unknown profiles fail; `release` is unavailable. `-Profile host` runs host tests.

`sdkconfig.defaults` and `partitions.csv` are versioned policy. ESP-IDF v6.1 has a
hidden `ESP_WIFI_ENABLED` SoC default: assigning it `n` does not disable Wi-Fi.
Instead MINIMAL_BUILD and explicit dependencies omit `esp_wifi` and `bt`.
The SD dependency graph includes the SDK's sole MSPI shim, but PSRAM support remains
disabled; the build script verifies that exact source exception and configuration.

FAT32 and exFAT use the same locked SDK FatFs. A project-local compiler overlay
enables exFAT consistently for the library and every consumer without editing the
SDK. ABI/config assertions fail closed if this assumption changes. Recording files
remain bounded below 10 MB; this is not unrestricted large-file VFS support.

## Authorized COM7 bench commands

Use the selected IDF Python environment, which supplies pyserial. The client is
deliberately fixed to COM7 and never enumerates or falls back to another port.
Only use it when this port is explicitly assigned to MC100. Build-generated
`flash_args` define the firmware addresses; back up the existing Flash before the
first overwrite. Flashing is not performed by these client commands.

```text
python firmware/tools/mc100_com7.py status
python firmware/tools/mc100_com7.py capture 3
python firmware/tools/mc100_com7.py record 3
python firmware/tools/mc100_com7.py list
python firmware/tools/mc100_com7.py download <final-filename> firmware/out/<new-local-file>
python firmware/tools/verify_recording.py <local.wav> <local.idx>
```

`capture` accepts 3–60 seconds and performs no SD I/O. `record` accepts 3–600
seconds including the first two seconds of real captured prerecord. The client
uses a nonce synchronization barrier, bounded replies and per-transfer CRC.
Downloads require new local paths under `firmware/out`; recordings and raw logs
are private test artifacts and must not be uploaded. Firmware never formats,
deletes or overwrites existing card files. Unsupported media prevents recording
but still allows capture-only diagnostics.

## Limits

Host tests cover core formats, state, queues, write transactions, protocol and
independent file validation. Compilation alone does not prove peripheral behavior.
Board observations are recorded separately; untested acoustic accuracy, battery
calibration, true power-loss recovery and long-run durability remain unqualified.
The `release` profile is intentionally unavailable.
