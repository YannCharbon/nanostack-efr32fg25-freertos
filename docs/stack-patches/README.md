# Patches proposed for nanostack-generic-standalone

**Status: both patches are applied in the stack** (GENERICITY_PLAN.md step S3),
and the firmware uses them: patch 0001 as the `NANOSTACK_FILE_NO_STDIO`
option, patch 0002 as `NANOSTACK_PORT` and
`nanostack_add_rcp_library(... INPROCESS)` (instead of the
`NANOSTACK_RCP_INPROCESS` option proposed below). The workarounds described
below are gone. The patch files are kept as a record.

None of these is required: the firmware builds and the host twin runs against
the **unmodified** stack. Each item is worked around inside this repository.
Each patch would remove a workaround once it lands in the stack through the
stack's own regression gates (GENERICITY_PLAN.md §4.3: G1 object identity for
the default Linux build, G2 link subset, G3 dag_simple runs where behaviour
could change).

Apply from the stack root with `patch -p1 < <file>`. Both were checked on a
scratch copy of the stack (2026-10-22): with 0001 + 0002 applied, a
consumer project cross-builds `nanostack_add_library(… ws_border_router
BORDER_ROUTER)`, `(… ws_router)` and `nanostack_add_rcp_library()` for
Cortex-M33 through `add_subdirectory()`:

- with its own port target;
- with mbedTLS from the consumer;
- without Threads;
- without the duplicate `rf_driver_storage.c`;
- with no stdio file references.

The default Linux build was not rebuilt here, because building inside the
stack repository was off-limits for this work. G1 still has to be run there.

| Patch | Plan item | What | Workaround used today |
| --- | --- | --- | --- |
| `0001-ns_file_system-optional-stdio-fallback.patch` | D9 | `NS_FILE_NO_STDIO`: compile out the stdio (`fopen`/`fread`/`fwrite`/`remove`/`rewind`/`fseek`/`ftell`) fallback of `ns_file_system.c` on targets without a file system. Default unchanged, so G1-neutral for the emulator. | The link wraps those calls onto failing stubs (`port/freertos/no_stdio_fs.c`, `-Wl,--wrap=…`). |
| `0002-cmake-selectable-port-and-inprocess-mac.patch` | D15, D18 | The CMake changes below. | `cmake/nanostack.cmake` reads the source lists out of the stack's `CMakeLists.txt` and builds the libraries itself, dropping `rcp/mac/rf_driver_storage.c`. |

Changes in patch 0002:

- `NANOSTACK_PORT` cache variable selects the port target. Default
  `nanostack_port_linux`; with another value the Linux port, Threads, backtrace
  and libdl are skipped.
- MbedTLS targets can be supplied by the consumer. This already worked through
  `if(NOT TARGET MbedTLS::mbedtls)`.
- `NANOSTACK_RCP_INPROCESS` drops the duplicate `rcp/mac/rf_driver_storage.c`
  for in-process builds.

## Findings that are not patches (for the stack/emulator owners)

1. **TLS entropy is registered as WEAK** (`tls_sec_prot_lib.c:142`,
   `MBEDTLS_ENTROPY_SOURCE_WEAK`).
   - Any port whose mbedTLS config has no strong source fails every EAP-TLS
     handshake ("drbg seed fail"). Examples: `MBEDTLS_NO_PLATFORM_ENTROPY`, as
     GENERICITY_PLAN.md §5.4 prescribes, without `MBEDTLS_ENTROPY_HARDWARE_ALT`.
   - The firmware fixes it in its mbedTLS config: the SE TRNG becomes a strong
     source via `mbedtls_hardware_poll()`.
   - Options for the stack: document the requirement in the porting API
     (§3.1.4), or register the port's random source as STRONG when the port
     declares it a TRNG.
   - The emulator is unaffected: it keeps the `/dev/urandom` platform source.
2. **`rcp/fhss/fhss_ws.c:1176`.** FHSS drift compensation is compiled only when
   `__linux__` is not defined. So the target and the emulator differ in
   behaviour, not only in traces; the host twin follows the emulator here.
   Other `__linux__` blocks in `rcp/mac` are debug traces only;
   `os_whiteboard.h` is a no-op off Linux.
3. **Channel mask.** `ws_management_channel_mask_set()` with channels 0..15
   (NA/1b) produced `channels: BC 129 UC 16`. In the host twin the RN then
   never got an ACK for its unicast EAPOL frames. Not investigated further: the
   mask may also need to reach the broadcast schedule, or the unicast DH1CF
   with excluded channels may disagree between nodes.
