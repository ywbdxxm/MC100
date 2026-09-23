# Task 2.3 report — portable supervisor boot skeleton

Date: 2026-09-23

## Scope

Implemented the portable `mc100_supervisor` component and host seam for the
Phase 2.3 boot path:

- startup recovery with a 30 s monotonic deadline;
- storage preparation of at least two writer slots before readiness;
- explicit battery and driver readiness callbacks, both fail-closed when
  absent or false;
- `READY(detail=2)` only after all prerequisites return successfully;
- `ARM` dispatch through the existing audio owner and explicit `ARMED`
  acknowledgement into `LISTEN`;
- recovery, preparation, and readiness failures enter `FAULT` and never reach
  `LISTEN`;
- public `tick` and dependency-injection APIs are present for Tasks 2.4–2.7.

The fake uses the existing in-memory storage adapter and counts the recovery
enumeration call through a complete forwarding `mc100_io_t` wrapper. It does
not open or access any serial port.

## Verification

The prescribed `firmware/tools/test-host.ps1 -Clean` command was attempted
before implementation but the shell PATH did not contain `cmake`, `ninja`, or
`ctest`; this environmental failure was recorded rather than treated as a
test result. After implementation, the installed Visual Studio host compiler
and project CMake/Ninja/CTest binaries were invoked explicitly:

```text
cmake -S firmware/host -B firmware/out/host-task23 -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build firmware/out/host-task23
ctest --test-dir firmware/out/host-task23 --output-on-failure
```

Result: 24/24 host tests passed, including `supervisor_boot`. The supervisor
test covers successful recovery/prepare/readiness to `LISTEN`, preparation
failure, battery false/missing, driver false, recovery timeout, and recovery
I/O failure; all failure paths assert `FAULT` and no `LISTEN`.

Target ESP-IDF build and hardware flashing were not run for this portable
host-only task. COM7 was not opened; no other COM port was touched.
