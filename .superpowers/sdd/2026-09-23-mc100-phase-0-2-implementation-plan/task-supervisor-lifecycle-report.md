# Supervisor lifecycle implementation report

## Files changed

- `firmware/components/mc100_supervisor/supervisor.c`: production lifecycle executor for ARM, OPEN, STOP_CAPTURE, CLOSE_THROUGH, RELEASE, HOLD, fault reporting, PCM/VAD ingestion, silence policy, queue draining, checkpointing, and recovery-safe destruction.
- `firmware/components/mc100_supervisor/include/mc100_supervisor.h`: read-only writer status seam used by host/target integration.

The existing test support and host registration edits in the worktree were preserved and were not staged by this task.

## Ownership and lifecycle decisions

- The supervisor is the single owner of state transitions and action execution. Audio remains the owner of PCM sequencing, rolling banks, snapshots, and the bounded packet queue; writer remains the owner of storage transactions, filename publication, and 15,000-frame rotation.
- STOP_CAPTURE reads the audio-reported cutoff validity and sequence, then CLOSE_THROUGH drains every accepted packet before closing. A false cutoff validity is preserved for the no-data path without fabricating a frame.
- VAD decisions are consumed per frame. After the trigger, 750 consecutive non-speech frames request one SILENCE_END; any speech decision resets the counter.
- MIC_IO, queue overflow, and healthy admission STORAGE_FULL follow the state machine's safe-prefix finalization path. Storage I/O, storage timeout, protocol, and other no-write faults abandon/release handles and complete HOLD quiescence.
- `mc100_supervisor_destroy` does not close or abandon an active writer; reset recovery therefore retains `.wav.part` evidence.

## Verification

Exact commands run:

```text
C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe --build firmware/out/host-msvc --config Debug --target mc100_supervisor test_supervisor_boot test_supervisor_record_open test_supervisor_record_active
firmware/out/host-msvc/Debug/test_supervisor_boot.exe
firmware/out/host-msvc/Debug/test_supervisor_record_open.exe
firmware/out/host-msvc/Debug/test_supervisor_record_active.exe
```

Results: supervisor library and three lifecycle targets compiled cleanly; boot, preroll OPEN, live queue/checkpoint, and rotation tests exited 0. The active test reported 300 frames written (100 preroll + 200 live) and 10 checkpoint syncs.

The full CTest suite and the close/fault/recovery additions were not run here because their registration and test edits are controlled by the parent integration task.

## Known limitations

- Final upload notification derives the published `.wav` basename from the writer status generation/segment and the injected boot identity; the writer remains the publication authority.
- This host-only verification does not prove ESP-IDF driver timing, SD latency, power behavior, or COM7 hardware behavior.

Commit: `12b2d2cc5226f379950b74b0ec8a13d7e4ff8044`
