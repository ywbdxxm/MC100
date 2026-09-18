# MC100 firmware infrastructure

This is the T01 boardless baseline, not a recorder. The application prints
`MC100 infrastructure only / recording not implemented` once, then blocks.
It does not enter LISTEN, initialize audio/SD/radio, or restart on a countdown.

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

The script invokes the validated installation's Python and `idf.py` with absolute
project, output, `SDKCONFIG`, defaults and target arguments. Outputs and generated
configuration stay under `firmware/out/target`; the user's `firmware/sdkconfig` and
existing `firmware/build` remain untouched. `-OutputDirectory out/<new-name>` supports
a separate output. Traversal outside `firmware/out`, output-root deletion and linked
output ancestors are rejected. `-Clean` deletes only the checked generated directory.
Unknown profiles fail; `release` is unavailable. `-Profile host` runs host tests.

`sdkconfig.defaults` and `partitions.csv` are versioned policy. ESP-IDF v6.1 has a
hidden `ESP_WIFI_ENABLED` SoC default: assigning it `n` does not disable Wi-Fi.
Instead MINIMAL_BUILD and explicit dependencies omit `esp_wifi`, `bt` and `esp_psram`;
the script checks this actual graph. Their unloaded Kconfig symbols are not assigned.

## Limits

The host test checks fixed board pin/capability and RAM-layout contracts; compilation
does not prove wiring or peripheral behavior. Target boot, Flash identity, USB operation,
PDM/SD/ADC/LED, current draw, memory/stack high-water marks, recording and all HIL gates
are NOT_RUN. Nothing here constitutes release readiness. Do not flash or infer a serial
port from this boardless baseline.
