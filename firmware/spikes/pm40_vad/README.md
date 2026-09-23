# MC100 Phase 1 PM/VAD spike

This directory is a throwaway measurement harness for the Phase 1 gate. It is
not part of the product image and must not be used to claim acoustic quality,
CPU headroom, or battery current without a target run and recorded evidence.

## Fixed identity

- Board contract: MC100 V1, ESP32-S3-MINI-1-N8, 8 MB flash, no external PSRAM.
- Target: `esp32s3`.
- Required SDK tuple: ESP-IDF `v6.1`, commit
  `fff9895c82d744c7237be8847347bdd1b07c6643`, as pinned by
  `firmware/dependencies.lock.json`.
- Probe input: 16 kHz mono, 16-bit PCM from the MC100 PDM RX pins defined by
  `mc100_board`.
- VAD candidate currently vendored for the probe: libfvad source snapshot
  `532ab666c20d3cfda38bca63abbb0f152706c369` (nested upstream checkout); this
  is not yet a product dependency or a VAD selection decision.

## Phase 1 deliverables and gates

| Gate | Evidence required | Current status |
| --- | --- | --- |
| Environment | Clean IDF checkout at the pinned commit; IDF-owned Python, tools, CMake, Ninja, and ESP32-S3 compiler all resolve inside the same installation tuple | **NOT RUN**: this shell has no `IDF_PATH`, `IDF_TOOLS_PATH`, or `IDF_PYTHON_ENV_PATH`; CMake/Ninja/compiler are not on PATH |
| Static/build | Build this project twice, using isolated output directories and `sdkconfig.defaults` plus `sdkconfig.pm80.defaults` / `sdkconfig.defaults` (40 MHz) | **NOT RUN**; requires the environment gate |
| PM/PDM | On the target, capture `PM_CONFIG`, `PM_CLOCK`, `PM_LOCK_STATS`, `PDM_INFO`, progress, and `SPIKE_SUMMARY`; repeat at requested 40 and 80 MHz | **NOT RUN**; requires target access and a serial session |
| VAD comparison | Run libfvad and an esp-sr VADNet probe with PSRAM disabled; record init result, internal heap/largest block, per-frame latency (P95), and stack high-water mark | **NOT RUN**; esp-sr is not present in the repository lock or this spike |
| Current | Measure battery-side current with a calibrated instrument for the same 40/80 MHz runs, recording setup and sample window | **NOT RUN**; no current measurement is implied by this document |
| Decision | Select a candidate only after buildability, no-PSRAM operation, resource/latency data, and authorized-corpus recall evidence are recorded | **OPEN** |

The target acceptance thresholds come from the Phase 1 plan and final design:
40 MHz reachability must be explicit; Phase 3 VAD acceptance is recall >=95%,
P95 decision latency <=300 ms, and <=3 false starts/hour. These thresholds are
not tested by the host/static checks below.

## Safe static checks completed

The following repository-only checks are safe without a board or serial port:

```powershell
Get-Content firmware/dependencies.lock.json
git -C firmware/spikes/pm40_vad/libfvad rev-parse HEAD
rg --files firmware/spikes/pm40_vad
```

They confirm the target/SDK declarations, the libfvad snapshot hash above, and
that the probe has separate 40/80 MHz defaults, a custom partition table, and
no product `app_main` integration. They do **not** compile or execute the
firmware.

## Reproducible run procedure (when the matching tuple is activated)

Use a new output directory for each profile. The project build must be done
with the pinned IDF Python and `idf.py`; do not use a bare ambient `idf.py`.
Build output is intentionally kept outside the source tree's product build.

```powershell
python $env:IDF_PATH/tools/idf.py -C firmware/spikes/pm40_vad `
  -B firmware/out/spike-pm40 `
  -D SDKCONFIG=firmware/out/spike-pm40/sdkconfig `
  -D SDKCONFIG_DEFAULTS="firmware/spikes/pm40_vad/sdkconfig.defaults;firmware/spikes/pm40_vad/sdkconfig.pm80.defaults" `
  -D IDF_TARGET=esp32s3 build
```

Repeat with a different output directory and `sdkconfig.defaults` (40 MHz),
then flash/monitor only under the parent task's explicit COM7 authorization.
Record the complete `SPIKE_SUMMARY` line and the instrument log together. Do
not enumerate ports or change any port other than COM7.

## Decision hygiene

Until the target and current logs exist, this spike must remain `NOT_RUN` and
the product power architecture and VAD choice remain unchanged. In particular,
the source code's requested frequency, heap counters, and timing fields are
measurement hooks, not measurements.
