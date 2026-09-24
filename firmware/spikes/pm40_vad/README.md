# MC100 Phase 1 PM/VAD spike

This directory is a throwaway measurement harness for the Phase 1 gate. It is
not part of the product image. The 2026-09-23 COM7 run below is target evidence
for this exact tuple only; it must not be used to claim acoustic quality,
product VAD selection, or battery current where the corresponding measurement
is still marked open.

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
| Environment | Clean IDF checkout at the pinned commit; IDF-owned Python, tools, CMake, Ninja, and ESP32-S3 compiler all resolve inside the same installation tuple | **PASS (2026-09-23)** for the target builds listed below: ESP-IDF v6.1, target `esp32s3`, pinned revision |
| Static/build | Build this project with isolated output directories and the 40/80/APB-lock/DFS defaults | **PASS**; build/flash logs are retained under `firmware/out/com7-phase1/` |
| PM/PDM | On the target, capture `PM_CONFIG`, `PM_CLOCK`, `PM_LOCK_STATS`, `PDM_INFO`, progress, and `SPIKE_SUMMARY`; repeat at requested 40 and 80 MHz | **PARTIAL / OPEN**: fixed 80 and DFS ran 10 s (501 frames, zero timeout/error); fixed 40 and fixed 40 + APB lock hang in `i2s_channel_enable()` and trigger Task WDT; DFS active PDM is 80/80 MHz |
| VAD comparison | Run libfvad and an esp-sr VADNet probe with PSRAM disabled; record init result, internal heap/largest block, per-frame latency (P95), and stack high-water mark | **PARTIAL / OPEN**: esp-sr 2.4.7 + VADNet1 medium **target build PASS**; COM7 runtime reached model discovery but AFE creation **INIT FAIL / BLOCKED** on internal `sr_rb_create` memory exhaustion; libfvad ran on the 80 MHz paths (P95 402/401 µs) |
| Current | Measure battery-side current with a calibrated instrument for the same 40/80 MHz runs, recording setup and sample window | **NOT RUN**; no current measurement is implied by this document |
| Decision | Select a candidate only after buildability, no-PSRAM operation, resource/latency data, and authorized-corpus recall evidence are recorded | **OPEN**: no VAD candidate selected; authorized corpus and current evidence are missing |

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
no product `app_main` integration. The target results are recorded separately
below; repository-only checks by themselves do **not** compile or execute the
firmware.

## 2026-09-23 COM7 target evidence

Only `COM7` was used for the target runs below. The chip identified as ESP32-S3 QFN56 revision v0.2 with
8 MB flash and USB Serial/JTAG; an 8 MB flash image was read before the probe.
The full evidence report is
[`docs/software/2026-09-23-mc100-pm40-vad-spike.md`](../../../docs/software/2026-09-23-mc100-pm40-vad-spike.md).

| Profile | Target observation |
| --- | --- |
| Fixed 40 MHz (`max=min=40`) | PM reported CPU/APB 40/40 MHz. PDM configuration printed, then `i2s_channel_enable()` did not return; the Task WDT backtrace decoded to `i2s_ll_rx_update`. No frame or `SPIKE_SUMMARY` was produced. |
| Fixed 40 MHz + explicit APB lock | Lock creation/acquire returned success, but clocks stayed 40/40 and the same PDM start hang occurred. |
| Fixed 80 MHz (`max=min=80`) | PDM ran for about 10 s: 501 frames, 320,640 bytes, zero timeouts/errors. libfvad initialized without PSRAM and reported P95 402 µs. |
| DFS (`max=80`, `min=40`) | PDM ran for about 10 s with the same frame/error result, but actual active CPU/APB stayed 80/80 MHz; 40 MHz was not observed during PDM. libfvad P95 was 401 µs. |

These data establish the fixed-40 PDM startup result for this ESP-IDF v6.1 /
ESP32-S3 tuple. They do not establish battery current, acoustic recall,
false-start rate, or a successful esp-sr runtime. Keep the Phase 1 decision gate
**OPEN** until an esp-sr configuration can complete initialization (or is
explicitly ruled out with the alternatives documented), a fair VAD comparison,
authorized-corpus metrics, and battery-side current are recorded.

## esp-sr VADNet target-build and COM7 runtime evidence

An isolated Component Manager throwaway probe resolved `espressif/esp-sr==2.4.7`
with `VADNet1 medium` enabled for `esp32s3`. The generated configuration has no
`CONFIG_SPIRAM`; the probe requests `AFE_MEMORY_ALLOC_MORE_INTERNAL`. The
target image build **PASS**ed: model packing reported `vadnet1_medium` at
281.16 KiB, `mc100_esp_sr_probe.bin` was generated (0x846a0 bytes), and the
3 MiB app partition retained 83% free. The original log and image artifacts
were generated under the ignored local
path `firmware/out/phase1-evidence/`; those artifacts are not retained in this
checkout.

The target image was then flashed and monitored on **COM7 only**. Model
discovery succeeded, but the selected no-PSRAM configuration could not create
the AFE:

```text
SR_MODEL_INIT result=OK count=1
SR_MODEL index=0 name=vadnet1_medium
SR_HEAP phase=before_afe internal_free=383384 internal_largest=319488 spiram_free=0
SR_RINGBUF: sr_rb_create: Memory exhausted
Guru Meditation Error: Core 0 panic'ed (StoreProhibited)
```

Runtime status is **INIT FAIL / BLOCKED**. The decoded backtrace is
`flash_model_info` → `model_create` → `afe_init_vad` →
`afe_create_from_config` → `app_main`; no AFE success, frame latency, stack
high-water mark, or PDM-fed VAD result was obtained. The runtime log was
generated under the ignored local path
`firmware/out/phase1-evidence/`; it is not retained in this checkout.
This is a failure of this `vadnet1_medium` / `AFE_MEMORY_ALLOC_MORE_INTERNAL`
/ no-PSRAM tuple, not proof that every esp-sr model or memory strategy is
impossible. Windows MAX_PATH required a short temporary build path, which does
not change the source or product dependency lock. The pre-flash 8 MiB backup
was retained; no other port or image was touched after the runtime probe.

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

Until esp-sr runtime is unblocked (or its alternatives are separately
evaluated), a fair comparison, corpus, and current logs exist, this spike must
remain `OPEN` and the product power architecture and VAD choice remain
unchanged. In particular, a successful target build, the source code's
requested frequency, heap counters, and timing fields are measurement hooks,
not runtime measurements.
