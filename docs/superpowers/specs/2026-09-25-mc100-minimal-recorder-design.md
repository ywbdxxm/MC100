# MC100 Minimal Recorder Design

**Date:** 2026-09-25

**Status:** Proposed for review

**Target:** MC100 V1, ESP32-S3-MINI-1-N8, 8 MB Flash, no PSRAM

**SDK:** ESP-IDF v6.1, commit `fff9895c82d744c7237be8847347bdd1b07c6643`

## Goal

Deliver one small, reproducible product path that starts recording at boot,
writes PCM to the microSD card while capture continues, stops after 600 seconds,
and leaves valid WAV files that can be downloaded and checked on a host.

The existing ESP-IDF peripheral adapters and the existing MC100 storage writer
remain the implementation base. This phase changes the runtime composition; it
does not redesign the on-card format.

## Current context

The EVT USB path has evidence for PDM capture at 80 MHz and SD recording on a
64 GB exFAT card. The autonomous product path has not completed a target serial port
`BOOT -> LISTEN -> RECORD -> CLOSE` smoke test. The repository also contains a
Supervisor, fixed VAD seam, pre-roll buffers, battery policy, upload seam,
journal/index recovery, and two application entry paths.

The first product milestone must close the gap between the proven EVT capture
path and a boot-driven recording path without activating those future features.

## Scope

### Included

- Boot-time board and SD initialization.
- PDM RX at 16 kHz, 16-bit, mono.
- A bounded producer/consumer path from PDM frames to the existing writer.
- Continuous writes while capture is running.
- A 600-second recording session.
- Existing writer rotation at approximately 300 seconds per segment.
- Normal WAV/index finalization at the 600-second boundary.
- Bounded handling of microphone, queue, storage, and timeout failures.
- Host and target serial port validation of the product profile.

### Deferred

- VAD selection and speech-triggered recording.
- Pre-roll and silence-based close.
- Wireless, upload, OTA, and network code.
- Automatic low-battery policy and battery-triggered stop.
- New journal, recovery, or file-format work.
- True power-loss recovery beyond preserving an unfinished `.part` file.
- Replacing the existing writer with a new storage library.

## Product behavior

1. `app_main` starts the minimal recorder task for the product profile.
2. The recorder initializes the board and mounts the card with bounded retries.
3. The recorder prepares the existing writer and starts the PDM RX owner task.
4. The PDM owner reads fixed-size PCM blocks and assembles 20 ms frames.
5. Frames enter a fixed-capacity queue. The storage owner removes frames and
   calls `mc100_writer_append`.
6. The capture owner counts frames accepted into the queue. At 30,000 frames it
   publishes a stop request and becomes quiescent; it does not enqueue frame
   30,001.
7. The storage owner confirms producer quiescence, drains only frames already
   in the queue, closes the writer normally, and then leaves the recorder in
   `IDLE`.
8. The recorder does not automatically start a second session.

After the final close, the recorder task remains alive in `IDLE` while retaining
the storage owner and mounted-card lifetime. No other task may access the
mounted adapter. This avoids leaving an owner handle associated with a deleted
FreeRTOS task and leaves a defined place for a later diagnostic/download mode.

The existing fixed VAD and Supervisor are bypassed for this path. V1 is
continuous recording, so a one-shot `vad_fixed(trigger_seq=120)` decision or a
silence-close threshold must not be able to stop the session. The battery wait
and monitor are also bypassed; the board has no verified software power-cut
signal for this milestone.

The existing writer's bounded segment format remains in force. A normal session
therefore produces two approximately five-minute `.wav` files and their `.idx`
sidecars. No new ten-minute single-file format is introduced in this phase.

## Runtime architecture

```text
PDM/I2S owner task
    -> mc100_assembler
    -> fixed PCM frame queue (producer)
    -> recorder/storage owner (consumer)
    -> mc100_writer
    -> FatFs / SDMMC
```

### Reused components

| Existing component | Reuse in V1 |
| --- | --- |
| `mc100_platform_espidf/audio_i2s.c` | ESP-IDF PDM setup, DMA, read, stop, overflow reporting |
| `mc100_platform_espidf/sd_fat.c` | ESP-IDF SDMMC/FatFs mount and file adapter |
| `mc100_audio/frame_assembler.c` | Convert byte reads into ordered 20 ms frames |
| `mc100_storage/wav.c` | WAV header encoding |
| `mc100_storage/writer.c` | Reservation, append, checkpoint, rotation, finalization |
| `mc100_storage/crc32.c` and index code | Existing writer integrity records |
| `mc100_board` | MC100 V1 pin and capability contract |

The recorder does not call FatFs or I2S from callbacks. Each owner task keeps
the existing single-owner contract for its peripheral and file handles.

The storage/recorder task owns SD mount, `writer_prepare`, `writer_begin`,
`writer_append`, `writer_close_through`, publication draining, and unmount. The
capture task is the only caller of `mc100_platform_audio_start/read/stop`.
Tasks communicate through the static frame queue and a bounded stop/fault event;
neither task directly touches the other task's peripheral or file handles.

### Excluded from the default product graph

The following remain available for later phases or diagnostics but are not
required by the V1 product runtime:

- `mc100_supervisor` and `mc100_core/state.c`;
- `product_runtime.c` monitor/audio command handshake;
- `vad_fixed.c`, `preroll.c`, and the pre-roll bank;
- `battery_policy.c` and automatic battery transitions;
- `upload_noop.c` and upload interfaces.

The EVT USB bench path remains a separately selectable diagnostic profile. It is
not the default product entry point and does not define product acceptance.

### Build graph

This is a build-graph boundary, not only a runtime convention. The minimal
product profile must not compile and link the legacy product runtime just to
leave it unused:

- Split `product_runtime.c` from the minimal platform component sources, or
  include it only under an explicit future-runtime Kconfig option.
- Make `mc100_platform_espidf` depend on `mc100_supervisor` only for that
  future-runtime option.
- Keep `mc100_core` as a narrow shared library because the reused writer and
  public result/types depend on its headers; do not claim that all of `core`
  can disappear in V1.
- Include `preroll.c` and `vad_fixed.c` in the audio component only for the
  future-runtime/EVT profile that needs them. The minimal product path uses the
  frame assembler and its own bounded queue.
- Keep the legacy Host tests available, but register them under a separate
  future-runtime test option rather than pulling the unused runtime back into
  the default product graph.

## Test scope

The Host suite is not an ESP32 or SD-card simulator. Its fake I/O keeps file
operations in memory and injects deterministic failures so the portable writer
can be tested without a card. The Windows file adapter checks host-file
interoperability. Neither adapter proves I2S DMA timing, SD electrical behavior,
FatFs behavior on a real card, or FreeRTOS scheduling.

The default V1 test set keeps the code that protects the selected path:

- WAV encoding and Python WAV interoperability;
- PCM frame assembly and capture-gap handling;
- writer prepare, append, rotation, finalization, short-write, and storage
  failure behavior;
- CRC/index codec behavior used by the reused writer;
- board, driver, and exFAT configuration contracts;
- downloaded-recording verification utilities.

Supervisor, fixed-VAD, pre-roll, battery, upload, product-audio-protocol, and
other future-runtime tests remain in the repository but are moved out of the
default V1 CTest registration. They can be enabled by a named future-runtime
test profile when those features re-enter the product path. This keeps the
default verification output proportional to the current requirement without
discarding tests for code intentionally retained for later phases.

## Buffer and timing contract

- Sample rate: 16,000 samples/second.
- Frame size: 320 samples, 640 PCM bytes, 20 ms.
- Queue capacity: reuse `MC100_STREAM_FRAMES` (96 frames, approximately 1.92
  seconds of PCM) unless target memory measurements require a documented change.
- Queue storage is statically allocated in internal RAM. A queue of 96
  `mc100_frame_t` values is approximately 62 KiB; the V1 path does not also
  allocate the existing pre-roll banks or Supervisor stream buffers.
- Static queue storage must use the FreeRTOS-required byte alignment. The
  implementation must not copy each frame into a second packet-sized buffer;
  target measurements must include the queue, task stacks, and high-water marks.
- No queue growth is permitted during recording. Record queue high-water and
  fault counters in the stop log.
- A queue full event is a terminal recording fault for the session. The capture
  owner stops at a safe point, and the writer closes the current file with an
  incident reason instead of silently dropping frames.
- The normal session limit is frame-count based: 30,000 accepted frames. A
  separate wall-clock deadline prevents a blocked driver or storage path from
  running forever; the 610-second deadline starts after PDM startup discard and
  the first accepted frame.
- PDM startup discard remains the existing 1,280 bytes (about 40 ms). It is
  initialization time, not part of the 600-second accepted-audio budget.
- A PDM read that returns `MC100_TIMEOUT` may still report a nonzero byte count.
  The producer must feed any reported bytes to the assembler before treating an
  empty timeout as an idle read; only a non-timeout error is immediately fatal.
- Writer checkpoints continue to use the existing implementation. No new
  buffering or background task is added solely for checkpointing.
- After each clean rotation or final close, the storage owner drains the
  writer's bounded publication queue and records the published names. V1 has no
  upload consumer, so leaving publications queued is not permitted.

## Failure behavior

### Boot or mount failure

The recorder reports a structured initialization failure, does not fabricate an
audio file, and enters `IDLE`/faulted state. It attempts board and SD
initialization at most three times with a one-second delay between attempts. It
does not format the card, delete existing files, or retry forever. Existing
stale `.part` and reserve files are
not silently deleted; the writer's existing prepare/diagnostic behavior remains
the authority for those names.

### PDM read failure or overflow

The capture owner stops producing frames. The recorder closes the active writer
as an incident/partial recording after the queue is quiescent. No frame after a
confirmed capture gap is presented as continuous audio.

### Queue overflow

The queue remains bounded. Overflow is latched as a session fault, capture is
stopped, and the current file is finalized as partial if the writer can still
perform a safe close.

After a producer error or queue overflow, the consumer may drain the already
queued safe prefix, but it must not call the normal clean close unless the
writer has a continuous final sequence. A no-frame session is abandoned or
released according to the writer API; it is never published as a clean WAV.

### SD or writer failure

The writer's existing failure state is preserved. The recorder stops capture
before releasing handles, drains only after the producer is quiescent, and does
not rename an unverified file to a clean `.wav` name.

### Hard power loss

The board has no software warning from the power switch. V1 makes no claim of
lossless power-fail recovery. A file interrupted by power loss may remain a
`.part` file; recovery remains a later phase.

## File and compatibility policy

- Keep the current WAV and index formats unchanged.
- Keep existing file naming and sidecar behavior.
- A normal ten-minute session is expected to leave two clean `.wav` files and
  two matching `.idx` files. The writer may also leave its preallocated reserve
  `.wav.part`/`.idx.part` slot; that is normal capacity preparation, not a third
  recording.
- `writer_prepare` may allocate two reserve slots before the first frame. Startup
  logs record the preparation result and elapsed time so a slow card is visible
  during target serial port validation.
- Do not add a third-party audio container or storage framework.
- Do not change the ESP-IDF component lock or board pin contract.
- Keep 64 GB exFAT support because it is part of the existing EVT evidence;
  simplify the application layer above FatFs rather than replacing the
  filesystem adapter.

## Validation gates

### Host

- Run the existing writer, WAV, assembler, and storage fault tests.
- Add focused tests for the 600-second session policy: 30,000 frames stop the
  session, 30,001 frames are not written, and a timeout produces an incident
  close.
- Add a queue-full test that confirms the producer stops and the writer does
  not publish a clean file after a dropped frame.
- Add a portable recorder-lifecycle test for producer-stop acknowledgement,
  consumer drain, two clean segment publications, no-frame close, and the
  producer-stop/consumer-drain race.
- Keep future-runtime tests available but exclude them from the default V1
  CTest command until their components are part of the product graph again.

### Target build

- Build the product profile with the pinned ESP-IDF v6.1 environment.
- Confirm the product image does not require VAD, Supervisor, upload, or network
  components for the default recording path.
- Record image size and internal-RAM queue/task budgets.

### target serial port smoke

- Use target serial port only.
- Boot with a known 64 GB exFAT card and capture the startup and stop logs.
- Verify that recording starts without a USB command.
- Verify two approximately five-minute clean files are present after the
  600-second session and that each has the expected WAV properties. Record each
  PCM byte count (approximately 9,600,000 bytes) and the combined count
  (approximately 19,200,000 bytes); account for the startup discard and stop
  boundary in the log.
- After the recorder is idle and the board is powered down, read the card
  through a PC card reader without formatting, deleting, or rewriting it. Run
  the existing recording verifier on both pairs; compare PCM byte counts and
  durations. V1 does not add a USB file-download protocol to the product image.
- Confirm the device remains idle after the final close and does not start a
  third clean file. The recorder task remains alive in `IDLE`; a reserve
  `.part` pair may remain.

### Fault checks

- Exercise a missing/unmountable card and verify a bounded initialization
  failure.
- Exercise a controlled writer failure in Host tests; do not deliberately cut
  power or alter the user's SD card during this phase.

## Success criteria

This phase is complete only when all of the following are true:

1. The product profile starts recording automatically after boot.
2. PDM capture and SD writes run concurrently through a bounded queue.
3. A normal 600-second session produces two valid WAV segments and matching
   sidecars.
4. The recorder stops after the session and does not restart automatically.
5. Host tests cover the duration boundary and queue-full behavior.
6. After power-down, a PC card reader and the existing verifier validate both WAV/IDX pairs.
7. No claim is made for VAD, wireless, battery shutdown, or true power-loss
   recovery.

## Risks and decisions held for later

- SD write latency may require tuning the queue capacity after target
  measurements; any change must remain statically bounded and be recorded.
- If the two-segment output is inconvenient for users, a later format revision
  can raise the single-file limit, but it must include a compatibility and
  recovery review rather than changing the constants silently.
- The existing writer is custom application code. Reusing it now limits risk;
  replacing it with a smaller standard-WAV writer remains a separate,
  evidence-backed decision after V1 works on hardware.
