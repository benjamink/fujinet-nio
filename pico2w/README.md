# NIO on the Raspberry Pi Pico 2 W

A first port of NIO's firmware to the **Pico 2 W** (RP2350, Cortex-M33, CYW43439
Wi-Fi). The portable core (`src/lib`), config store, devices, Wi-Fi, raw TCP/UDP/DNS,
TNFS, USB CDC, UART console, LittleFS, TLS and HTTP/HTTPS build and link for the
board. Nothing here
has run on hardware yet.

It follows the architecture rule: no `#ifdef` in shared code. The port is a new
`src/platform/pico/` plus a `main_pico.cpp`, selected by being a different
project, as `bridges/*` are.

## Build

```sh
source scripts/env.sh                      # exports PICO_SDK_PATH (AUR pico-sdk)
cmake -S pico2w -B /tmp/nio-pico2w-build
cmake --build /tmp/nio-pico2w-build -j8
```

Needs `arm-none-eabi-gcc` and `arm-none-eabi-newlib`, and the Pico SDK with its
lwIP, mbedTLS and CYW43 submodules. The first configure downloads picotool.
The default build type is `MinSizeRel` (`-Os`). The result is
`/tmp/nio-pico2w-build/fujinet_nio_pico2w.uf2`.

## Size and acceptance

The final build uses **1102836 bytes flash** and **173288 bytes static RAM**.
Core 0 reserves a 16 KiB stack. This is a build measurement, not a runtime peak:
core objects, protocol objects, TLS records/certificates and queues also allocate
from the heap. A live TLS connection's peak heap and stack usage remains unknown.
The board has 520 KiB SRAM and 4 MiB flash; the last MiB is reserved for LittleFS.
See the ordered measurements below for exact baselines and component totals.

## Backend status

| Area | State |
|---|---|
| Core, devices, config, services | Real (`src/lib`) |
| Build profile | `src/lib/build_profile/rp2350_pico2w.cpp` (FujiBus over USB CDC) |
| Wi-Fi link | Real: `src/platform/pico/wifi_link.cpp`, CYW43 through `cyw43_arch`. No scan, no BSSID pinning |
| Sockets (TCP, UDP, DNS) | Raw lwIP: bounded receive queues, opaque addresses, TCP listener/client and TNFS TCP/UDP |
| TCP protocol | Portable TCP state machine over raw lwIP |
| TLS, HTTP | ALTCP/mbedTLS verified TLS; bounded HTTP/1.x request/response adapter |
| Channel | TinyUSB CDC for FujiBus; UART0 is reserved for logs/console |
| Filesystem | LittleFS `flash` on final 1 MiB, including persistent `fujinet.yaml` |
| Console | Cooperative diagnostics and filesystem shell over UART0 |
| Memory stats | Approximate (newlib `mallinfo`) |
| Threads | Shims: `compat/mutex` is a real Pico SDK mutex; `compat/condition_variable` polls; no service tasks |
| Time, logging | Shared with `src/platform/posix` (portable newlib code) |

### Sockets

Raw lwIP (`NO_SYS=1`) avoids a scheduler and BSD socket emulation. Foreground
calls acquire the CYW43 lwIP lock; callbacks run under the background dispatcher's
lock. Eight TCP handles each hold a 4096-byte receive ring; four UDP handles each
hold four 1472-byte datagrams. Stale handles never alias reused slots. TCP advances
its receive window only when bytes are consumed; `TCP_WND=4096` prevents lwIP from
delivering a coalesced packet larger than the ring. Writes accept only available
send-buffer space. Oversized UDP packets are dropped; short reads discard the
remaining packet. IPv4 is supported; IPv6 is not enabled.

The lwIP heap is 24 KiB, with 8 active TCP PCBs, 2 listeners, and 32 queued TCP
segments; send buffers are two MSS per connection. These are bounded starting
budgets for runtime measurement, not throughput claims. Four stable DNS request
slots survive the 5-second foreground timeout until their late callbacks finish.
TNFS TCP uses the portable TCP protocol state machine, avoiding the older shared
TcpChannel's BSD `select` dependency. A failed partial TNFS send closes the stream.


### Filesystem

LittleFS persists `fujinet.yaml` on onboard flash. TNFS and HTTP filesystems use
the implemented network backends. SD over SPI remains future work.

### Memory

`lwipopts.h` bounds socket state and lwIP pools. `mbedtls_config.h` enables TLS
1.2 with 16 KiB records, certificate dates and hostname checks. Runtime heap and
stack measurements with a live TLS connection are still required.

## Left out of the build, and why

| File | Reason |
|---|---|
| `src/lib/diagnostic_uart_channel_provider.cpp` | Includes an ESP32 header from `src/lib` |
| `src/lib/fs_stdio.cpp` | Needs `<dirent.h>`, which newlib does not have |
| `src/lib/hotsync/*` | Needs service tasks; the HotSync stub is used |
| `src/lib/visor_usb/*` | Visor USB host profile |

The `compat/` headers shadow `<mutex>` and `<condition_variable>` because
arm-none-eabi libstdc++ has no gthreads. `src/platform/pico/libc_shims.cpp`
provides `sleep`/`usleep` for `std::this_thread::sleep_for`.

## Hardware acceptance still required

Verify Wi-Fi/DNS/TCP/UDP traffic, USB enumeration and full/disconnect recovery,
UART diagnostics, flash persistence/power-loss recovery, and trusted/untrusted/
wrong-hostname/expired TLS servers. Measure peak heap and stack with a TLS
connection open. Optional UART transport, scan/BSSID selection, SNTP, low-water
memory statistics and a core-1 task were not added.

## Ordered port measurements (build verification only)

GCC 16.2 / SDK 2.3.1, MinSizeRel. Bytes, not rounded KB. Flash is the generated
`.bin`; static RAM is `.data + .bss + .ram_vector_table + .uninitialized_data`.
`arm-none-eabi-size`'s default Berkeley columns classify Pico `.data` as text,
so use `arm-none-eabi-size -A` and the linker map instead.

| Stage | Flash bytes | Static RAM bytes | Reserved heap/stack bytes |
|---|---:|---:|---:|
| Baseline skeleton (remeasured) | 855364 | 68884 | 2048 / 2048 |
| 1: sockets and TNFS | 934792 | 146836 | 2048 / 2048 |
| 2: USB and console | 995432 | 163152 | 2048 / 2048 |
| 3: LittleFS | 1016616 | 164124 | 2048 / 2048 |
| 4: TLS and HTTP/HTTPS | 1102836 | 173288 | 2048 / 16384 |

Map component totals (flash/static RAM input bytes; excludes linker alignment):

| Component | Baseline | Sockets |
|---|---:|---:|
| CYW43 | 236622 / 2598 | 236622 / 2598 |
| NIO core | 219292 / 181 | 231320 / 181 |
| NIO platform/app | 7341 / 560 | 70480 / 57600 |
| SDK/TinyUSB | 26877 / 5210 | 26877 / 5210 |
| libstdc++ | 182383 / 4473 | 182383 / 4473 |
| lwIP | 27218 / 45302 | 31376 / 66212 |
| newlib/libgcc | 79190 / 8121 | 79190 / 8121 |
| yaml-cpp | 72306 / 1993 | 72306 / 1993 |
| Other | 2178 / 20 | 2178 / 20 |

Reproduce component accounting by summing allocated input sections after the
map's `Linker script and memory map` heading, grouping object paths/archives;
count `.data` in both flash and RAM, `.bss` only in RAM, and code/rodata only in
flash. Do not count discarded sections or output-section totals again.

Run the raw callback mock harness with `cmake -S tests/pico -B /tmp/nio-pico-raw-tests`,
`cmake --build /tmp/nio-pico-raw-tests`, and `ctest --test-dir /tmp/nio-pico-raw-tests`.
Nine cases cover TCP callback lifetime, refused pbuf ownership, EOF/error, partial
writes, listener guards, UDP boundaries, and DNS late callback/timeout exhaustion.
Native socket-state tests cover wrap/full buffers, stale handles, packet boundaries,
empty and truncated datagrams. The external POSIX PTY build passes all six CTest
checks, and the configured ESP32 bench S3 firmware links. No hardware acceptance,
radio throughput, flash endurance, or TLS runtime heap/stack measurements exist.
Static RAM remains a lower bound; it does not establish that a TLS connection fits.

### USB and console ownership

One full-speed CDC ACM interface carries FujiBus only. `pico_stdio_usb` is disabled;
stdio logs and console use UART0, GPIO0 TX/GPIO1 RX, 115200 baud, 3.3 V logic.
TinyUSB is serviced before and after every core tick. Its RX/TX FIFOs are 4096 bytes;
the channel queues at most 16384 additional TX bytes and submits at most 64 bytes
per service call. Writes are atomic: overflow quarantines the stream and clears
FIFOs until the host disconnects/reconnects. Connection epochs invalidate partial
protocol state. UART console output queues 8192 bytes, truncates excess with an
explicit marker, and drains only while UART is writable. Logs share UART output.
Blocking DNS/TNFS operations can delay USB servicing by up to their bounded timeout;
USB acceptance/latency measurements are pending hardware.

The descriptor currently uses Raspberry Pi's development VID/PID 2e8a:000a;
a production identity must be assigned before release. Board unique ID supplies
its serial. UART channel transport is optional future work, separate from console.

USB-stage map flash/RAM input bytes: CYW43 236622/2598, core 241145/181,
platform/app 121939/65950, SDK/TinyUSB 25878/13174, libstdc++ 182383/4473,
lwIP 31376/66212, newlib/libgcc 79386/8121, yaml-cpp 72306/1993, other 2178/20.
Native Pico helper selection passes 4 cases/49 assertions, including bounded USB
writes, ordering, overflow quarantine, and reconnect queue isolation. Pico ELF/UF2
links; USB enumeration and UART console interaction remain untested on hardware.

### Flash partition and failure policy

The first 3 MiB (offset 0x000000–0x2fffff) are firmware. LittleFS exclusively owns
0x300000–0x3fffff (256 erase blocks of 4096 bytes, 256-byte program pages).
`flash_limit.ld` fails the link if `__flash_binary_end` crosses 0x10300000, and all
block callbacks independently reject overflow, misalignment and out-of-partition
requests. Programs copy one page into RAM before `flash_safe_execute`; erase and
program execute with interrupts disabled and core 1 declared inactive. Do not
launch core 1 without replacing that declaration with coordinated flash lockout.
USB/radio service can pause during erase; hardware timing remains unmeasured.

LittleFS uses 256-byte caches, a 32-byte lookahead and `block_cycles=500` wear-level
metadata relocation. File flush commits through LittleFS; files retain volume
ownership until closed. A blank partition is formatted only after every byte is
verified 0xff. Nonblank mount failure, read failure, or format failure leaves the
filesystem unavailable; corruption is never silently reformatted. Back up or
explicitly erase the partition using a maintenance tool before reinitializing it.

The native `tests/pico` target uses the real LittleFS C implementation over bounded
RAM flash. Five cases (21055 assertions) pass: persistence across remount, handles
outliving filesystem objects, directory/file operations, corrupt first/tail bytes,
I/O failures, and checked page/block boundaries. Firmware links with the partition
guard. Power-loss recovery, endurance and physical writes remain hardware pending.

Flash-stage map input bytes (flash/RAM): CYW43 236622/2598, LittleFS 15002/0,
core 241145/181, platform/app 127116/65950, SDK/TinyUSB 26646/14099,
libstdc++ 182383/4473, lwIP 31376/66212, newlib/libgcc 79560/8159,
yaml-cpp 72306/1993, other 2178/20.

### TLS trust, time and memory

The SDK ALTCP TLS adapter is used directly. Its default optional authentication
is overridden with `MBEDTLS_SSL_VERIFY_REQUIRED`; the hostname is installed before
connect and verification flags are checked before marking the stream connected.
TLS 1.2 supports ECDHE RSA/ECDSA with P-256/P-384, 16 KiB records, two concurrent
TLS connections and a 15-second handshake deadline. Certificate configuration
outlives the connection; callbacks are detached before close/abort frees state.

Default platform trust is the committed ISRG Root X1 certificate, **not a full
browser/system CA store**. Choose a maintained PEM bundle with
`-DNIO_PICO_CA_BUNDLE=/absolute/path/roots.pem` when other roots are required.
`-DFN_HTTPS_TEST_CA=ON` trusts only the existing FujiNet test CA;
`-DFN_HTTPS_TEST_CA_ADDITIVE=ON` adds it to the platform bundle. These options are
mutually exclusive. There is no verification-disabled fallback.

Certificate validity checks are enabled. Set the clock through the existing
ClockService SetTime API after every boot; until then TLS open returns NotReady.
Pico time advances from the SDK monotonic clock after an accepted UTC set.
SNTP is not implemented and network time sync returns false. The platform
setter accepts 2020–2100; certificate validity remains a separate mbedTLS check.

RP2350 `pico_rand` uses its TRNG. The port owns `mbedtls_hardware_poll`, copying
only the remaining entropy length. SDK 2.3.1's callback copies eight bytes for
some shorter final chunks; its interface source is excluded, with native canary
tests for lengths 0–32. Hardware SHA alternatives are not enabled.

Core 0's default 4 KiB scratch bank cannot hold a 16 KiB stack. SDK memory-location
overrides reserve 0x2007c000–0x2007ffff for that stack, with `__HeapLimit` at
0x2007c000 and `__StackTop` at 0x20080000. The old physical scratch-Y bank is unused.
The 16 KiB reservation is a starting budget, **not a measured stack high-water
mark**; TLS crypto executes in the background lwIP callback context. Static RAM
excludes each TLS object's 16 KiB receive ring, mbedTLS records/certificates,
request/read replay caches, the USB channel object and other core heap use.

### HTTP API and bounds

GET, POST, PUT, DELETE and HEAD are implemented over TCP or TLS. Request headers
and response metadata are bounded to 8 KiB, URLs to 2048 bytes, individual header
lines to 2048 bytes, chunk-size lines to 128 bytes, and header counts to 64.
Known request bodies use `bodyLenHint` as Content-Length. Existing flag bit 2
with zero length enables chunked upload through a bounded 4096-byte chunk buffer;
a zero-length write commits the upload. Caller-supplied Transfer-Encoding and
Expect headers are rejected. Reads/writes are sequential with replay of the last
accepted block; capability methods advertise the sequential requirement.

Responses support Content-Length, chunked decoding (including bounded trailers),
close-delimited bodies, HEAD/204/304, and up to four informational responses.
Conflicting framing, invalid chunk boundaries, malformed headers and early EOF
produce errors. Bodies stream through an 8 KiB queue; response sizes over the
32-bit stream-offset range are rejected. Backpressure pauses transport reads.
There is a 15-second inactivity deadline while room exists to receive data;
a full application queue resets that budget while the client is paused.
Only allowlisted response headers appear in NetworkInfo.

With follow_redirects, at most three redirects are followed. Cross-origin
redirects strip Authorization/Cookie; HTTPS-to-HTTP downgrade is rejected.
303 and POST 301/302 switch to GET. A redirect requiring replay of an uploaded
body returns Unsupported instead of retaining an unbounded upload. Compression
is not negotiated or decompressed. These limits are explicit API errors, not
silent truncation.

TLS/HTTP-stage map input bytes (flash/RAM): CYW43 236622/2598, LittleFS 15002/0,
core 238790/181, platform/app 145553/65970, SDK/TinyUSB 29378/14103,
libstdc++ 182463/4473, lwIP 33826/66571, mbedTLS 63601/8776,
newlib/libgcc 80664/8159, yaml-cpp 72306/1993, other 2178/20.

### Reproduce native verification

Source the workspace environment first. Configure/build/ctest these independent
native projects outside the repository:

- `tests/pico` → `/tmp/nio-pico-raw-tests`: raw callback and actual LittleFS tests.
- `tests/pico-http` → `/tmp/nio-pico-http-tests`: 10 cases / 4519 assertions covering
  parser framing, malformed responses, methods/uploads/replay, redirects,
  timeout and a 20 KiB response across queue backpressure.
- `tests/pico-tls` → `/tmp/nio-pico-tls-tests`: 3 cases / 1342 assertions using the
  actual SDK mbedTLS and firmware configuration for trusted/wrong-host/unknown-CA/
  future/expired certificates, entropy boundaries and Pico clock progression.
- `tests/pico-tls-adapter` → `/tmp/nio-pico-tls-adapter-tests`: three builds test
  default/test-only/additive trust selection, hostname setup, fail-closed
  verification/time, callback detachment, partial writes, limits and timeout.

For example, `cmake -S tests/pico-http -B /tmp/nio-pico-http-tests`, then
`cmake --build /tmp/nio-pico-http-tests -j8` and
`ctest --test-dir /tmp/nio-pico-http-tests --output-on-failure`.
These tests exercise native logic and mocked hardware boundaries; they are not
USB/radio/flash hardware or live-TLS peak measurements. Raw and LittleFS targets
also pass AddressSanitizer/UndefinedBehaviorSanitizer (including vendor C).
