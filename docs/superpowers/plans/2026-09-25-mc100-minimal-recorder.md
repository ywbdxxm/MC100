# MC100 Minimal Recorder Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the product profile start recording at boot, write continuously to the SD card for 600 seconds through the existing writer, produce two valid approximately five-minute WAV segments, and then remain idle.

**Architecture:** Add a small portable recorder-session policy and a target-only recorder loop. The capture task owns PDM/I2S and feeds a statically aligned 96-frame queue; the recorder task owns board/SD mount and the existing writer, drains the queue after producer quiescence, and closes files. Gate the legacy Supervisor/VAD/product runtime and their tests out of the default product graph while keeping their source available for a future profile.

**Tech Stack:** ESP-IDF v6.1 (`fff9895c82d744c7237be8847347bdd1b07c6643`), ESP32-S3-MINI-1-N8 without PSRAM, C11, FreeRTOS, ESP-IDF I2S PDM, SDMMC/FatFs, CMake/Ninja/CTest, MSVC host tests, COM7 validation.

**Spec:** `docs/superpowers/specs/2026-09-25-mc100-minimal-recorder-design.md`

## Global Constraints

- Product target is `esp32s3`, 8 MB Flash, no PSRAM, and the locked ESP-IDF v6.1 tuple in `firmware/dependencies.lock.json`.
- Product recording starts automatically after successful board and SD initialization; no USB start command and no VAD decision are required.
- The session accepts exactly 30,000 20 ms frames, nominally 600 seconds; frame 30,001 is rejected.
- PDM startup discard remains 1,280 bytes; a `MC100_TIMEOUT` with nonzero `actual` bytes still feeds the assembler.
- The frame queue is statically aligned internal RAM with 96 `mc100_frame_t` items; it never grows and never duplicates each frame into a packet buffer.
- I2S calls stay in the capture owner task. FatFs and writer calls stay in the recorder/storage owner task.
- Queue full, capture gap, writer error, and deadline expiration never publish a clean WAV; they produce an incident/partial close when the writer API permits it.
- Normal writer rotation and the existing `.wav`/`.idx` format remain unchanged. A reserve `.wav.part`/`.idx.part` pair may remain after preparation.
- VAD, pre-roll, Supervisor, battery wait/monitor, upload, wireless, and true power-loss recovery are not in the default product path.
- Never format, delete, or overwrite unrelated SD-card files during validation. Use COM7 only for board/serial operations; read the card with a PC card reader after power-down.

---

### Task 1: Establish the V1 test profile and baseline

**Files:**

- Modify: `firmware/tools/test-host.ps1`
- Modify: `firmware/host/CMakeLists.txt`
- Test: existing `firmware/tests/` targets registered by the host CMake file

**Interfaces:**

- Add PowerShell switch `-FutureRuntimeTests`.
- Add CMake option `MC100_ENABLE_FUTURE_RUNTIME_TESTS` default `OFF`.
- Default CTest keeps board/driver contracts, WAV/interoperability, journal codec, frame assembly, assembler-gap behavior, writer/storage fault behavior, recording verifier, and the new V1 recorder tests.
- The option gates Supervisor, battery, upload, fixed-VAD, pre-roll, product-audio-protocol, recovery-only, and EVT-command tests.

- [ ] **Step 1: Add the host switch before changing registrations.**

In `test-host.ps1`, add:

```powershell
[switch]$FutureRuntimeTests
```

Append `-DMC100_ENABLE_FUTURE_RUNTIME_TESTS=ON` to `$configureArgs` only when
`$FutureRuntimeTests` is present. Leave the existing output-path and tool checks
unchanged.

- [ ] **Step 2: Gate only future-runtime registrations.**

Surround the target creation and `add_test` calls for these exact names with
an `if(MC100_ENABLE_FUTURE_RUNTIME_TESTS)` block with a matching `endif()`:
`state`,
`battery_policy`, `upload_noop`, `preroll_handoff`, `queue_full`, `capture_gap`,
`vad_fixed`, `supervisor_boot`, `supervisor_record_open`,
`supervisor_record_active`, `supervisor_edges`, `supervisor_monitor_events`,
`supervisor_rearm`, `supervisor_audio_owner`, `supervisor_close`,
`supervisor_fault`, `supervisor_recover_roundtrip`,
`supervisor_rotation_publication`, `product_audio_protocol`, `evt_commands`,
`recover_prefix`, and `recover_idempotent`.

Also define the host `mc100_audio` source list as `frame_assembler.c` by
default. Append `audio.c`, `preroll.c`, `stream.c`, and `vad_fixed.c` only when
`MC100_ENABLE_FUTURE_RUNTIME_TESTS` is enabled. This keeps the default Host
library aligned with the minimal target audio path.

Keep `mc100_format`, `mc100_writer`, `mc100_fake_io`, `test_wav`,
`wave_interop`, `journal_codec`, `frame_assembler`, `storage_writer`,
`storage_short_write`, `rotate_15000`, `space_admission`, `finalize_faults`,
`writer_publication`, `board_contract`, `driver_contract`,
`recording_verifier`, and `com7_client` available to the default profile.

- [ ] **Step 3: Run the default host configure and list tests.**

Run:

```powershell
pwsh -File firmware/tools/test-host.ps1 -Clean
ctest --test-dir firmware/out/host -N
```

Expected: configure/build succeeds in a provisioned MSVC/CMake environment;
the list contains no `supervisor_*`, `vad_fixed`, `battery_policy`,
`upload_noop`, or `product_audio_protocol` tests. If the local machine lacks
CMake, record the environment failure and continue the plan in the pinned host
environment; do not install tools from this script.

- [ ] **Step 4: Verify the future profile still exposes retained tests.**

Run:

```powershell
pwsh -File firmware/tools/test-host.ps1 -Clean -FutureRuntimeTests
ctest --test-dir firmware/out/host -N
```

Expected: the legacy Supervisor/VAD/battery/upload test names are present. Do
not claim they validate the V1 product path.

- [ ] **Step 5: Commit the test-profile change.**

```powershell
git add firmware/tools/test-host.ps1 firmware/host/CMakeLists.txt
git commit -m "test: split MC100 V1 and future runtime host tests"
```

### Task 2: Add the portable recorder-session policy and lifecycle tests

**Files:**

- Create: `firmware/components/mc100_recorder/CMakeLists.txt`
- Create: `firmware/components/mc100_recorder/include/mc100_record_session.h`
- Create: `firmware/components/mc100_recorder/record_session.c`
- Create: `firmware/tests/test_record_assembler_gap.c`
- Create: `firmware/tests/test_record_session.c`
- Modify: `firmware/host/CMakeLists.txt`

**Interfaces:**

The new portable component must expose exactly these concepts without FreeRTOS,
I2S, FatFs, or ESP-IDF headers:

```c
enum {
    MC100_RECORD_DURATION_SECONDS = 600,
    MC100_RECORD_FRAME_MS = 20,
    MC100_RECORD_TARGET_FRAMES =
        MC100_RECORD_DURATION_SECONDS * 1000 / MC100_RECORD_FRAME_MS,
    MC100_RECORD_DEADLINE_SECONDS = 610
};

typedef struct {
    uint64_t target_frames;
    uint64_t enqueued_frames;
    uint64_t consumed_frames;
    uint64_t deadline_ms;
    bool started;
    bool producer_quiesced;
    bool faulted;
    mc100_result_t fault;
} mc100_record_session_t;

void mc100_record_session_init(mc100_record_session_t *session);
void mc100_record_session_start(mc100_record_session_t *session,
                                uint64_t first_frame_ms);
bool mc100_record_session_can_enqueue(
    const mc100_record_session_t *session);
mc100_result_t mc100_record_session_note_enqueued(
    mc100_record_session_t *session);
mc100_result_t mc100_record_session_note_consumed(
    mc100_record_session_t *session);
void mc100_record_session_mark_producer_quiesced(
    mc100_record_session_t *session);
void mc100_record_session_mark_fault(mc100_record_session_t *session,
                                     mc100_result_t reason);
bool mc100_record_session_target_reached(
    const mc100_record_session_t *session);
bool mc100_record_session_deadline_expired(
    const mc100_record_session_t *session, uint64_t now_ms);
bool mc100_record_session_can_clean_close(
    const mc100_record_session_t *session);
```

`note_enqueued` must reject the 30,001st frame with `MC100_FULL`; it is called
only after the FreeRTOS queue accepts a frame. `can_clean_close` is true only
when the target was reached, the producer is quiescent, every queued frame was
consumed, and no fault was latched. A no-frame or faulted session can never
clean-close.

- [ ] **Step 1: Write the failing policy/lifecycle tests.**

Implement test functions named `target_boundary`, `deadline_is_bounded`,
`producer_must_quiesce_before_close`, `queue_fault_forbids_clean_close`, and
`no_frame_cannot_clean_close` in `test_record_session.c`. Also add
`test_record_assembler_gap.c`, which feeds 639 bytes, calls
`mc100_assembler_capture_gap`, and asserts that later input remains rejected.
The boundary test
must call `note_enqueued` exactly 30,000 times, assert the target is reached,
then assert the next call returns `MC100_FULL`. The close test must assert false
before producer quiescence and before consumption of all enqueued frames.

The assembler-gap test must include this case:

```c
static mc100_result_t collect(void *context, const mc100_frame_t *frame) {
    (void)context;
    (void)frame;
    return MC100_OK;
}

mc100_assembler_t assembler;
uint8_t pcm[640] = {0};
mc100_assembler_init(&assembler, 0);
assert(mc100_assembler_feed(&assembler, pcm, 639, collect, NULL) == MC100_OK);
assert(mc100_assembler_capture_gap(&assembler) == MC100_IO);
assert(mc100_assembler_feed(&assembler, pcm, sizeof(pcm), collect, NULL) == MC100_IO);
```

Run:

```powershell
cmake --build firmware/out/host --target test_record_session
```

Expected: configuration fails because the new target does not exist yet.

- [ ] **Step 2: Implement the state machine.**

Implement the header contract in `record_session.c` with no allocation and no
global mutable state. Preserve the first fault reason, saturate no counters,
and calculate the deadline as `first_frame_ms + 610000` with overflow-safe
comparison.

- [ ] **Step 3: Register and run the focused test.**

Add `mc100_recorder` as a static host library, link `test_record_session` to it,
link `test_record_assembler_gap` to a minimal host `mc100_audio` library that
contains only `frame_assembler.c`, apply the existing `mc100_host_checks`
function, and register:

```cmake
add_test(NAME record_session COMMAND test_record_session)
add_test(NAME record_assembler_gap COMMAND test_record_assembler_gap)
```

Run:

```powershell
cmake -S firmware/host -B firmware/out/host -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build firmware/out/host --target test_record_session
ctest --test-dir firmware/out/host -R '^record_session$' --output-on-failure
```

Expected: all lifecycle assertions pass.

- [ ] **Step 4: Commit the portable policy.**

```powershell
git add firmware/components/mc100_recorder firmware/tests/test_record_session.c firmware/host/CMakeLists.txt
git commit -m "feat: add bounded MC100 recording session policy"
```

### Task 3: Make the ESP-IDF build graph select the minimal product

**Files:**

- Modify: `firmware/main/Kconfig.projbuild`
- Modify: `firmware/main/CMakeLists.txt`
- Modify: `firmware/components/mc100_platform_espidf/CMakeLists.txt`
- Modify: `firmware/components/mc100_audio/CMakeLists.txt`
- Modify: `firmware/components/mc100_core/CMakeLists.txt`
- Modify: `firmware/components/mc100_recorder/CMakeLists.txt`

**Interfaces:**

- `CONFIG_MC100_APP_PRODUCT=y` remains the product profile selector.
- Add `CONFIG_MC100_LEGACY_RUNTIME`, default `n`, which is valid only with the
  product profile and gates the old `product_runtime.c`/Supervisor path.
- Product builds compile `app_main.c` and `record_loop.c`; EVT builds compile
  `app_main.c`, `evt_capture.c`, and `evt_commands.c`.
- Minimal product audio sources include `frame_assembler.c`; `audio.c`,
  `stream.c`, `preroll.c`, and `vad_fixed.c` are included only for EVT or
  `CONFIG_MC100_LEGACY_RUNTIME`.
- Minimal platform sources are `audio_i2s.c`, `sd_fat.c`, and `board_io.c`.
  `product_runtime.c` and `mc100_supervisor` are conditional legacy sources.
- Minimal `mc100_core` is header-only for the target; `state.c`,
  `battery_policy.c`, and `upload_noop.c` remain available to EVT/legacy
  profiles. Host builds continue to compile their explicit test libraries.

- [ ] **Step 1: Add the legacy-runtime Kconfig switch.**

Add to `Kconfig.projbuild`:

```kconfig
config MC100_LEGACY_RUNTIME
    bool "Build the legacy Supervisor product runtime"
    default n
    depends on MC100_APP_PRODUCT
```

Update the existing product help text to describe the 600-second minimal
recorder as the default product behavior.

- [ ] **Step 2: Make `main/CMakeLists.txt` select sources and dependencies.**

Build the source list before `idf_component_register` and use these complete
branches:

```cmake
set(MC100_MAIN_SRCS "app_main.c")
if(CONFIG_MC100_APP_PRODUCT)
    list(APPEND MC100_MAIN_SRCS "record_loop.c")
    idf_component_register(
        SRCS ${MC100_MAIN_SRCS}
        PRIV_REQUIRES freertos mc100_board mc100_core mc100_storage
                      mc100_audio mc100_platform_espidf mc100_recorder
                      esp_hw_support heap
        INCLUDE_DIRS "")
else()
    list(APPEND MC100_MAIN_SRCS "evt_capture.c" "evt_commands.c")
    idf_component_register(
        SRCS ${MC100_MAIN_SRCS}
        PRIV_REQUIRES freertos mc100_board mc100_core mc100_storage
                      mc100_audio mc100_platform_espidf esp_hw_support heap
        INCLUDE_DIRS "")
endif()
```

Keep the existing EVT private requirements in the `else()` branch and add
`mc100_recorder` to the product branch. Do not link `mc100_supervisor` from the
minimal product.

- [ ] **Step 3: Condition platform, audio, core, and recorder components.**

Use CMake source lists and `if(CONFIG_MC100_LEGACY_RUNTIME)` blocks. The
minimal platform must not list `product_runtime.c` or `mc100_supervisor` in its
`REQUIRES`. The minimal recorder component must require only the portable core
and expose its include directory. The target core component must call
`idf_component_register(INCLUDE_DIRS "include")` without legacy source files
when the minimal product is selected.

- [ ] **Step 4: Build both target profiles far enough to inspect the graph.**

Run in the validated ESP-IDF environment:

```powershell
pwsh -File firmware/tools/build.ps1 -Profile product -Clean
pwsh -File firmware/tools/build.ps1 -Profile evt -Clean
```

Inspect `firmware/out/target/project_description.json` and confirm the product
source/component list excludes `product_runtime.c` and `mc100_supervisor`,
while the EVT profile still contains `evt_capture.c` and the diagnostic audio
sources.

- [ ] **Step 5: Commit the build-graph change.**

```powershell
git add firmware/main/Kconfig.projbuild firmware/main/CMakeLists.txt firmware/components/mc100_platform_espidf/CMakeLists.txt firmware/components/mc100_audio/CMakeLists.txt firmware/components/mc100_core/CMakeLists.txt firmware/components/mc100_recorder/CMakeLists.txt
git commit -m "build: select minimal MC100 product graph"
```

### Task 4: Implement the target recorder loop

**Files:**

- Create: `firmware/main/record_loop.h`
- Create: `firmware/main/record_loop.c`
- Modify: `firmware/main/app_main.c`
- Test: `firmware/tests/test_record_session.c` and target build

**Interfaces:**

Expose one product entry point:

```c
void mc100_record_run(void);
```

The function never returns during normal operation. After the final close it
keeps the storage owner task alive in `IDLE` and delays at one-second intervals.

- [ ] **Step 1: Add the aligned static queue and ownership context.**

Define one `record_runtime_t` containing the `StaticQueue_t`, a byte buffer
declared with `_Alignas(portBYTE_ALIGNMENT)`, the queue handle, capture task
handle, stop/fault flags, `mc100_record_session_t`, writer handle, and mounted
state. The queue item type is exactly `mc100_frame_t`; do not add a second
`mc100_packet_t` buffer.

- [ ] **Step 2: Implement the producer callback and capture task.**

The callback must:

1. Reject enqueue when `mc100_record_session_can_enqueue` is false.
2. Call `xQueueSend(runtime->frame_queue, frame, 0)` and latch `MC100_FULL`
   if it fails.
3. Call `mc100_record_session_start` on the first successfully queued frame,
   using `mc100_platform_now_ms()` at that frame.
4. Call `mc100_record_session_note_enqueued` after queue acceptance.
5. Notify the storage owner when the target or a fault is reached.

The capture loop must follow this exact read rule:

```c
mc100_result_t r = mc100_platform_audio_read(
    pcm, sizeof(pcm), &actual, 100);
if (r != MC100_OK && r != MC100_TIMEOUT) {
    latch_fault(r);
    break;
}
if (actual != 0) {
    r = mc100_assembler_feed(&assembler, pcm, actual,
                             enqueue_frame, runtime);
    if (r != MC100_OK) {
        latch_fault(r);
        break;
    }
}
```

An empty timeout is an idle read and does not fabricate a frame. On stop or
fault, call `mc100_platform_audio_stop` from this task, mark the producer
quiescent, and notify the storage owner. No other task calls I2S.

- [ ] **Step 3: Implement the storage owner lifecycle.**

In `mc100_record_run`, perform board init and at most three SD mount attempts
with one-second delays. Generate the boot ID, create the writer, call
`mc100_writer_prepare`, and start the capture task only after storage is ready.
Use generation `1` for the one session. When the first frame is dequeued, call
`mc100_writer_begin` with that frame sequence, then append it and count it as
consumed.

On writer append failure, latch the first fault and notify the producer to stop.
Wait for producer quiescence before draining the queue. For each drained frame,
call `mc100_writer_append` only while the writer is healthy and count successful
consumption through `mc100_record_session_note_consumed`.

- [ ] **Step 4: Finalize according to the writer contract.**

After producer quiescence and queue drain:

```c
if (mc100_record_session_can_clean_close(&session)) {
    mc100_writer_close_through(writer, 1, last_seq, 0);
} else if (writer_active && writer_has_frames) {
    mc100_writer_close_through(writer, 1, last_seq, incident_reason);
} else {
    mc100_writer_abandon(writer, incident_reason);
    mc100_writer_release_handles(writer);
}
```

Handle the return value at every call. Drain `mc100_writer_publication_pop`
after each rotation and after finalization, logging each clean publication;
the V1 path has no upload consumer. Do not publish a clean file after a queue
full, capture gap, writer failure, deadline, or invalid final sequence.

- [ ] **Step 5: Enter IDLE and update `app_main`.**

Change the product branch in `app_main.c` to create the recorder task and call
`mc100_record_run`. Remove its reference to `mc100_product_run`. Keep the EVT
branch unchanged. After finalization, log session counts, queue peak, fault,
segment names, and `IDLE`, then keep the recorder task alive while the card
owner remains mounted.

- [ ] **Step 6: Build the product profile.**

Run:

```powershell
pwsh -File firmware/tools/build.ps1 -Profile product -Clean
```

Expected: product image builds with no `product_runtime.c`, Supervisor, VAD, or
network component in the default graph and with the static queue/task memory
reported for review.

- [ ] **Step 7: Commit the recorder runtime.**

```powershell
git add firmware/main/record_loop.h firmware/main/record_loop.c firmware/main/app_main.c
git commit -m "feat: add boot-driven bounded MC100 recorder"
```

### Task 5: Synchronize documentation and validation records

**Files:**

- Modify: `README.md`
- Modify: `firmware/README.md`
- Modify: `docs/MC100-SOFTWARE.md`
- Modify: `docs/MC100-VALIDATION.md`

**Interfaces:**

- Documentation must state that product mode is boot-driven, 600 seconds,
  two approximately five-minute segments, and idle afterward.
- Documentation must distinguish product validation from EVT USB validation.
- Documentation must state that VAD, battery automation, wireless, and true
  power-loss recovery remain open/deferred.

- [ ] **Step 1: Update the software entry-path description.**

Replace the statement that the default product path is
`product_runtime -> supervisor` with the minimal recorder path and link to the
new recorder-session policy. Keep the EVT USB path as a diagnostic profile.

- [ ] **Step 2: Update the validation table before hardware testing.**

Add an explicit row for `product boot -> 600 s record -> idle` with status
`NOT RUN` until COM7 evidence exists. State that a normal run expects two clean
WAV/index pairs and may leave a reserve `.part` pair.

- [ ] **Step 3: Update build and test commands.**

Document:

```powershell
pwsh -File firmware/tools/test-host.ps1 -Clean
pwsh -File firmware/tools/test-host.ps1 -Clean -FutureRuntimeTests
pwsh -File firmware/tools/build.ps1 -Profile product -Clean
pwsh -File firmware/tools/build.ps1 -Profile evt -Clean
```

Explain that the first Host command is the V1 default and the second is only
for retained future-runtime tests.

- [ ] **Step 4: Run Markdown link and whitespace checks.**

Use the repository link checker and:

```powershell
git diff --check
```

Expected: no broken local links and no whitespace errors.

- [ ] **Step 5: Commit the synchronized documentation.**

```powershell
git add README.md firmware/README.md docs/MC100-SOFTWARE.md docs/MC100-VALIDATION.md docs/MC100-HARDWARE.md
git commit -m "docs: describe MC100 minimal recorder validation"
```

### Task 6: Execute the verification gates and record evidence

**Files:**

- Create: `evidence/2026-09-25-mc100-minimal-recorder/serial-com7.log`
- Create: `evidence/2026-09-25-mc100-minimal-recorder/recording-verification.json`
- Modify: `docs/MC100-VALIDATION.md`
- Modify: `docs/reports/2026-09-25-mc100-minimal-recorder.md`

**Interfaces:**

- Board operations use COM7 only.
- Card verification happens after power-down through a PC card reader and uses
  `firmware/tools/verify_recording.py`; it must not modify card contents.

- [ ] **Step 1: Run the default Host suite.**

Run:

```powershell
pwsh -File firmware/tools/test-host.ps1 -Clean
ctest --test-dir firmware/out/host --output-on-failure
```

Expected: all default V1 tests pass, including `record_session`, writer
faults, WAV interoperability, and frame assembly. Record the exact test count.

- [ ] **Step 2: Run the pinned Target product build.**

Run:

```powershell
pwsh -File firmware/tools/build.ps1 -Profile product -Clean
```

Record the target, SDK revision, image size, component list, queue bytes, task
stack sizes, and static-analysis/build result. Do not treat this as hardware
validation.

- [ ] **Step 3: Flash and observe COM7.**

Using the existing procedure in `docs/hardware/MC100-PROGRAMMING.md`, flash the
product image through COM7 and capture boot output for at least 11 minutes.
Confirm logs show board/SD preparation, automatic recording start, one clean
rotation, final close, publication names, and `IDLE` without a third clean
file. Record queue high-water, drops/faults, and task stack high-water marks.

- [ ] **Step 4: Verify the card contents read-only.**

Power down the board, remove the card without formatting or deleting files, and
copy only the two clean WAV/index pairs and any reserve `.part` pair to the
evidence directory. Run:

```powershell
python firmware/tools/verify_recording.py evidence/2026-09-25-mc100-minimal-recorder/segment0.wav evidence/2026-09-25-mc100-minimal-recorder/segment0.idx
python firmware/tools/verify_recording.py evidence/2026-09-25-mc100-minimal-recorder/segment1.wav evidence/2026-09-25-mc100-minimal-recorder/segment1.idx
```

Expected: both clean pairs validate as `FINAL`, each WAV is 16 kHz/16-bit/mono,
each segment is approximately 9,600,000 PCM bytes, and the combined duration is
approximately 600 seconds after accounting for startup discard and the exact
stop boundary.

- [ ] **Step 5: Run controlled Host fault cases.**

Run the queue-full, writer-short-write, storage-full, and no-frame-close tests.
Do not deliberately cut power to the board or modify the user's SD-card data
for this phase.

- [ ] **Step 6: Update evidence and status.**

Record the serial log, verifier JSON, card identity, image hash, exact SDK
tuple, and any deviations in the new report. Change the validation row to
`PASS` only for behaviors directly shown by these artifacts; leave VAD,
battery, power-loss, endurance, and wireless rows unchanged.

- [ ] **Step 7: Commit the evidence metadata and final status.**

```powershell
git add docs/MC100-VALIDATION.md docs/reports/2026-09-25-mc100-minimal-recorder.md evidence/2026-09-25-mc100-minimal-recorder
git commit -m "test: record MC100 minimal recorder validation"
```

## Final verification checklist

- [ ] `git status --short --branch` is clean.
- [ ] `git diff --check` is clean.
- [ ] Default Host CTest passes and future-runtime tests are separately gated.
- [ ] Product Target build passes with the locked ESP-IDF tuple.
- [ ] Product source/component graph excludes the legacy runtime by default.
- [ ] COM7 log shows automatic start, two clean segments, final stop, and IDLE.
- [ ] Both card pairs pass the read-only recording verifier.
- [ ] No claim is made for deferred VAD, battery shutdown, power-loss recovery,
  endurance, or wireless behavior.
