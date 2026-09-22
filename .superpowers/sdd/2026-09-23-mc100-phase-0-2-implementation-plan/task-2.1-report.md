# Task 2.1 implementation report

## Scope

Implemented the notification-only upload sink seam. The public `mc100_upload_sink_t`
contains the callback and opaque context, and `mc100_upload_noop()` returns a safe
no-op callback. The implementation performs no I/O, persistence, serial-port
enumeration, or upload-state handling.

## Changes

- Added `firmware/components/mc100_core/include/mc100_upload.h`.
- Added `firmware/components/mc100_core/upload_noop.c`.
- Added and registered `firmware/tests/test_upload_noop.c`.
- Registered the source in both the ESP-IDF component and host `mc100_core` library.

## TDD evidence

The test was added and registered before the production header/source. The required
host command was then attempted:

`pwsh -File firmware/tools/test-host.ps1 -Clean`

It could not configure because this environment has no `cmake` executable (the script
stops with “Missing cmake”). `git diff --check` passes. No host-suite pass can be
claimed until CMake/Ninja/CTest are available.

## API compatibility

No conflicting existing upload API was found. The requested callback/context layout
was used verbatim; `mc100_types.h` remains included as required by the project brief.
