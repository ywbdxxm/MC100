# Task 0.1 report — merge validated power-loss recovery core

Date: 2026-09-23
Worktree: `phase0-2/implementation`

## Commands and results

1. Read the Task 0.1 brief and checked the worktree. The worktree was clean before the merge; `main` was not checked out or mutated.
2. Conflict check:

   ```text
   base=$(git merge-base HEAD codex/mc100-recovery)
   git merge-tree "$base" HEAD codex/mc100-recovery
   ```

   Result: `NO CONFLICTS`.

3. Merged the recovery branch into the current feature branch:

   ```text
   git merge --no-ff codex/mc100-recovery -m "feat: merge validated power-loss recovery core"
   ```

   Result: clean `ort` merge. Merge commit: `0de9acc047526979bf17fb2f591fe529bfb2f912` (`0de9acc`). Parents: `aa812dd06423727f8fa93c6c2be9580dc2992c16` and `b49ed103dfac2836a639e69ff7276f131c816e29`.

4. Confirmed test registration:

   ```text
   git show codex/mc100-recovery:firmware/host/CMakeLists.txt | Select-String -Pattern recover
   ```

   The file contains the recovery source and the `recover_prefix` / `recover_idempotent` executable and `add_test` registrations. Task 0.1a was not needed.

5. Activated the x64 MSVC Developer Command Prompt through `VsDevCmd.bat` and ran:

   ```text
   pwsh -NoProfile -File firmware/tools/test-host.ps1 -Clean
   ```

   Configure and build succeeded. CTest completed with 21/21 tests passed (0 failed), total test time 6.25 seconds.

## CTest results

All passed: `board_contract`, `wav`, `journal_codec`, `wave_interop`, `state`, `battery_policy`, `frame_assembler`, `preroll_handoff`, `queue_full`, `capture_gap`, `storage_writer`, `storage_short_write`, `rotate_15000`, `space_admission`, `finalize_faults`, `recover_prefix`, `recover_idempotent`, `driver_contract`, `evt_commands`, `com7_client`, and `recording_verifier`.

## Concerns

- `VsDevCmd.bat` emitted a non-fatal warning that `vswhere.exe` was not recognized; CMake found MSVC 19.44.35228.0 and the complete build/test run still passed.
- No serial port was opened or touched.
- The generated `firmware/out/host` build output is ignored and did not dirty the worktree.
