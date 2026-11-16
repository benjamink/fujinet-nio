# NIO on the Raspberry Pi Pico 2 W

A first port of NIO's firmware to the **Pico 2 W** (RP2350, Cortex-M33, CYW43439
Wi-Fi). The portable core (`src/lib`), config store, devices, Wi-Fi, raw TCP/UDP/DNS,
TNFS, USB CDC, UART console, LittleFS, TLS and HTTP/HTTPS build and link for the
board. Nothing here has run on hardware yet.

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
The default build type is `MinSizeRel` (`-Os`).
`FN_LOG*` compile to nothing unless `FN_DEBUG` is defined, as on the other targets, so
the default image is silent on UART0. Configure with `-DFN_DEBUG=ON` to get the boot
banner and logs on UART0 (GPIO0 TX / GPIO1 RX, 115200 baud); it adds about 19.5 KB.

**Do not measure performance on a `FN_DEBUG` build.** It hex-dumps up to 512 bytes of every
FujiBus reply and request payload to UART0, which at 115200 baud takes about 250 ms per
reply. On hardware this made TCP reads look 100 times slower than they are (about 280 ms per
read against 4 ms, and 5 KB/s against 200 KB/s). Use the default build for any timing. The result is
`/tmp/nio-pico2w-build/fujinet_nio_pico2w.uf2`.

## Size and acceptance

The current build uses **1052624 bytes flash** and **349740 bytes static RAM**.
Core 0 reserves a 16 KiB stack and core 1 reserves 4 KiB. This is a build measurement, not a runtime peak:
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

Each UDP socket queues **four** datagrams and drops the rest. The UDP channel reports that
depth (`recv_queue_depth()`), and the TNFS client keeps no more read-ahead requests in flight
than that, so a burst of replies is never lost. Without it a TNFS read took about 108 ms per
512-byte block (a 184 KB file in 39 s) because half of each read-ahead burst was dropped and
the client waited out a 400 ms timeout; with room for the replies it takes 1.7 s. The tables
holding the sockets must start all-zero so they live in `.bss`: a non-zero default (a `-1`
"free" marker) put 106 KB of them in the firmware image.

Raw lwIP (`NO_SYS=1`) avoids a scheduler and BSD socket emulation. Foreground
calls acquire the CYW43 lwIP lock; callbacks run under the background dispatcher's
lock. Four TCP handles each hold a 20480-byte receive ring; four UDP handles each
hold four 1472-byte datagrams. Stale handles never alias reused slots. TCP advances
its receive window only when bytes are consumed. `TCP_WND=20480` holds a full
16 KiB TLS record plus up to 2053 bytes of expansion/header: SDK ALTCP returns
application-record receive credit only after decoding the entire record. The raw
ring matches that window so a coalesced packet always fits. Four raw handles
bound static RAM; a listener consumes one, leaving at most three accepted children. Writes accept only available
send-buffer space. Oversized UDP packets are dropped; short reads discard the
remaining packet. IPv4 is supported; IPv6 is not enabled.

The lwIP heap is 128 KiB, with 8 active TCP PCBs, 2 listeners, and 32 queued TCP
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
memory statistics were not added.

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
| Final hardening (TLS window and lifecycle fixes) | 1153112 | 222312 | 2048 / 16384 |
| TLS allocator pool prerequisite | 1153056 | 328816 | 2048 / 16384 |
| Dual-core USB worker | 1156328 | 349740 | 2048 / 20480 |
| Console `format` command | 1158152 | 349740 | 2048 / 20480 |
| Socket tables in `.bss`, not `.data` | 1052624 | 349740 | 2048 / 20480 |

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
TinyUSB initialization, USB IRQs and polling belong exclusively to core 1. Its
RX/TX FIFOs are 4096 bytes; the synchronized bridge adds 4096 RX bytes and at most
16384 TX bytes (including partially submitted staging). Each worker iteration
transfers at most 64 bytes per direction. Full RX leaves bytes in CDC for endpoint
backpressure; short and zero TX acceptance retains unsent bytes. Writes are atomic: overflow quarantines the stream and clears
FIFOs until the host disconnects/reconnects. Session transitions invalidate bridge RX/TX buffers and expose a connection
epoch. Shared FujiBus framer state does not yet consume that epoch. UART console output queues 8192 bytes, truncates excess with an
explicit marker, and drains only while UART is writable. Logs share UART output.
Core 0 owns NIO, Wi-Fi/lwIP/TLS, filesystems and UART console. Blocking network
operations on core 0 can overlap USB progress on core 1 at the stock 150 MHz.
There is no measured speedup or doubling-of-performance claim; physical USB
acceptance/latency measurements are pending.

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
program execute with core 0 interrupts disabled and core 1 paused by the SDK
multicore flash lockout. The USB worker registers as a lockout victim before
TinyUSB initialization and readiness publication. Core 0 must receive readiness
before mounting flash; initialization failure or a 2-second readiness timeout
returns from boot without mounting. The SDK ROM launch handshake precedes this
application timeout. Application messages never use the intercore FIFO.
USB/radio service can pause during erase; hardware timing remains unmeasured.

LittleFS uses 256-byte caches, a 32-byte lookahead and `block_cycles=500` wear-level
metadata relocation. File flush commits through LittleFS; files retain volume
ownership until closed. A blank partition is formatted only after every byte is
verified 0xff. Nonblank mount failure, read failure, or format failure leaves the
filesystem unavailable; corruption is never silently reformatted. Back up or
explicitly erase the partition using a maintenance tool before reinitializing it.

That policy is strict. The first Pico 2 W brought up on hardware carried a single
stray 256-byte page of 0xEF at the very end of flash (0x103fff00), which was enough to
disable persistence at boot (`Corrupted dir pair at {0x0, 0x1}` followed by `existing
nonblank storage was not formatted`; the config then lives in memory only). The
operator's way out is the console command `format flash confirm`: it recreates an
empty LittleFS superblock and reboots. It does not wipe the other blocks, as
LittleFS erases a block before writing it, so it is not a secure erase. Without
`confirm` it only prints a usage line and a warning.

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
request/read replay caches and other core heap use. USB bridge queues are fixed
static storage; only the small Channel facade is dynamically allocated.

### HTTP API and bounds

GET, POST, PUT, DELETE and HEAD are implemented over TCP or TLS. Request headers
and response metadata are bounded to 8 KiB, URLs to 2048 bytes, individual header
lines to 2048 bytes, chunk-size lines to 128 bytes, and header counts to 64.
Known request bodies use `bodyLenHint` as Content-Length. Existing flag bit 2
with zero length enables chunked upload through a bounded 4096-byte chunk buffer;
a zero-length write commits the upload; its successful acknowledgement remains
replayable after response headers arrive. Caller-supplied Transfer-Encoding and
Expect headers are rejected. Reads/writes are sequential with replay of the last
accepted block; capability methods advertise the sequential requirement.

Responses support Content-Length, chunked decoding (including bounded trailers),
close-delimited bodies, HEAD/204/304, and up to four informational responses.
Conflicting framing, invalid chunk boundaries, malformed headers and early EOF
produce errors. Bodies stream through an 8 KiB queue; response sizes over the
32-bit stream-offset range are rejected. Backpressure pauses transport reads.
There is a 15-second inactivity deadline while room exists to receive data;
a full application queue resets that budget while the client is paused. Waiting
for the caller to provide another upload block also pauses this deadline, but
only after request headers and any queued chunk have been sent and before final
response headers arrive. Stalled network
writes still time out; an idle caller can retain bounded connection state until
explicit close.
Only allowlisted response headers appear in NetworkInfo.

With follow_redirects, at most three redirects are followed. Cross-origin
redirects strip Authorization, Proxy-Authorization and Cookie; HTTPS-to-HTTP downgrade is rejected.
Relative redirects normalize dot segments and preserve the query on fragment-only
references. 303 and POST 301/302 switch to GET; the last successful upload block
remains replayable, but the resulting GET rejects new upload bytes. A redirect requiring replay of an uploaded
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
- `tests/pico-http` → `/tmp/nio-pico-http-tests`: 14 cases / 4851 assertions covering
  parser framing, malformed responses, methods/uploads/replay, redirects,
  timeout and a 20 KiB response across queue backpressure.
- `tests/pico-tls` → `/tmp/nio-pico-tls-tests`: 3 cases / 1342 assertions using the
  actual SDK mbedTLS and firmware configuration for trusted/wrong-host/unknown-CA/
  future/expired certificates, entropy boundaries and Pico clock progression.
- `tests/pico-tls-adapter` → `/tmp/nio-pico-tls-adapter-tests`: three builds test
  default/test-only/additive trust selection, hostname setup, fail-closed
  verification/time, callback detachment, partial writes, limits and timeout,
  plus receive credit/refusal/replay/EOF/error: 5 cases / 58 assertions per build.

For example, `cmake -S tests/pico-http -B /tmp/nio-pico-http-tests`, then
`cmake --build /tmp/nio-pico-http-tests -j8` and
`ctest --test-dir /tmp/nio-pico-http-tests --output-on-failure`.
These tests exercise native logic and mocked hardware boundaries; they are not
USB/radio/flash hardware or live-TLS peak measurements. Raw and LittleFS targets
also pass AddressSanitizer/UndefinedBehaviorSanitizer (including vendor C).

### Measurement report and remaining acceptance

Run the checked-in report tool against the matching ELF, map and binary:

```sh
python3 pico2w/size_report.py /tmp/nio-pico2w-build/fujinet_nio_pico2w.elf.map
python3 pico2w/size_report.py /tmp/nio-pico2w-build/fujinet_nio_pico2w.elf.map --json
python3 tests/test_pico_size_report.py
```

It parses the ARM ELF as data; it never runs it. Component totals count initialized
RAM in both flash and RAM, exclude discarded sections and omit linker padding.
The binary and allocated ELF sections supply the authoritative physical totals.
The minimum heap reservation is not a cap on heap usage. The earlier reproducible
[build-only record](measurements/2026-11-15-build.json) identifies the base commit and changes committed alongside it,
and explicitly leaves runtime peaks null. That historical record reports 1153112 flash / 222312 static RAM bytes. The
current dual-core record and its per-core stack accounting are below.

Final host validation: all six external POSIX CTest checks pass (881 native cases).
The existing subprocess disk test timed out in two runs during compilation;
its isolated 149-assertion retry and final full run with an idle toolchain passed.
The ESP32 bench S3 build passed after the separate prerequisite switch fix.
Generated POSIX and ESP32 source lists were inspected for Pico leakage and then
restored byte-for-byte. No submodule pins were changed and nothing was pushed.

- [x] Link Pico ELF/UF2 and measure each required backend stage.
- [x] Verify socket/USB/flash/HTTP logic and TLS trust/time policy natively.
- [x] Preserve POSIX/ESP32 compatibility and source-list isolation.
- [ ] Physical USB, Wi-Fi, flash and TLS acceptance (no board attached, confirmed by maintainer).
- [ ] Measure peak heap and stack with one live TLS connection; decide runtime fit in 520 KiB.
- [ ] Optional UART transport, scan/BSSID selection, SNTP and memory low-water marks.

Final hardening verification adds full-window raw receive/backpressure, FIFO accept
ordering and handle exhaustion, deadlines across the 32-bit millisecond boundary,
16 KiB TLS receive/credit/refusal/replay and queued data before EOF/reset, upload
commit replay, redirect credentials/paths, and stalled upload network writes.
USB mount/unmount and CDC line-state callbacks invalidate session state even if
multiple transitions occur within one task pump; queue-level edge tests cover
this invariant. Actual USB reset/reconnect timing still requires hardware.
The TLS window fix increases static RAM by 49024 bytes over stage 4; runtime heap
and stack peaks remain unmeasured, so this is not a runtime-fit claim.

### TLS pool sizing prerequisite (2026-11-15)

SDK ALTCP installs lwIP's allocator for mbedTLS, so the old 24 KiB pool could
not allocate even one pair of default 16 KiB TLS record buffers. The bounded
pool is now 128 KiB. `tests/pico-tls` includes actual SDK `mem.c` and ALTCP
allocator tests with the firmware mbedTLS configuration: parsing ISRG Root X1,
allocating adapter state, and setting up two contexts succeeds; an old-pool
negative control fails as expected. All allocations are freed back to zero.
This is host setup evidence, not a complete handshake, fragmentation margin,
concurrent socket traffic budget, or physical peak measurement.

Reproduce: source the workspace environment, then `cmake -S tests/pico-tls
-B /tmp/nio-pico-tls-tests`, `cmake --build /tmp/nio-pico-tls-tests -j8`, and
`ctest --test-dir /tmp/nio-pico-tls-tests --output-on-failure` (3/3 passed).
The separately linked [TLS pool measurement](measurements/2026-11-15-tls-pool.json)
preserves the earlier measurements and includes the enlarged static lwIP pool.

### Dual-core USB verification (2026-11-15)

The [dual-core build record](measurements/2026-11-15-dual-core.json) reports both
stack reservations. Linker symbols put core 0 at 0x2007c000–0x20080000 and core 1
at 0x20080000–0x20081000 (scratch-X); `__HeapLimit` is 0x2007c000. Core 1's 4 KiB
budget requires physical high-water validation under reset/reconnect and flash
activity. Both cores remain at the board's stock 150 MHz.

One permanent bridge owns all queue, fault, epoch and attachment state under a
Pico SDK mutex. Channel destruction detaches without leaving a pointer in the
worker; a second simultaneous channel is rejected. Core 1 holds the mutex only
for bounded queue/FIFO work, never across `tud_task`, waits, flash operations,
logging or application callbacks. Core 0 never holds it while requesting flash
lockout, so a paused worker cannot block the flash operation. No application or
network code, C++ allocation or logging runs on core 1. TinyUSB task callbacks
invalidate the session and clear CDC buffers immediately. A non-claiming app
class driver observes `configuration_reset` in exact task event order, before
CDC resets its FIFOs. It never claims an interface or endpoint; no IRQ-side
flag is needed. A reset and reconnect in the same `tud_task` preserves newly
received data. Staged bytes count against TX capacity
and are discarded on detach, reset, disconnect or overflow quarantine.

Source the workspace environment before these native gates:

- `cmake -S tests/pico -B /tmp/nio-pico-raw-tests`, build with `-j4`, then
  `ctest --test-dir /tmp/nio-pico-raw-tests --output-on-failure`: 6/6 pass, including
  production worker/channel adapters with core ownership, lockout registration,
  startup failure/timeout, reconnect, ordered reset, stale responses, attachment
  and teardown mocks.
- `cmake -S tests/pico-usb -B /tmp/nio-pico-usb-tests`, build with `-j4`, then
  `ctest --test-dir /tmp/nio-pico-usb-tests --output-on-failure`: 6 cases pass,
  including RX backpressure, atomic overflow quarantine, short/zero TX submit,
  staged session isolation and two threads exchanging 100000 ordered bytes in
  each direction through bounded queues.
- `python3 -m unittest discover -s tests -p test_pico_size_report.py`: 3 pass,
  including separate stack accounting without counting stacks as static RAM.

Pico ELF/UF2 links. Source-list generation was run, POSIX/ESP32 lists were
inspected for Pico leakage, and incidental template rewrites were restored.
USB pauses during flash erase/program while core 1 is safely locked out; pause
duration, physical concurrency, reconnect timing, sustained throughput, and
both stacks' runtime peaks remain unmeasured. No hardware was available.

Review hardening adds an atomic read-epoch capture and conditional response
write. A request that blocks on core 0 across a disconnect/reconnect cannot
send its response to the new USB session; only a nonempty read adopts that
session. Querying the epoch or performing an empty read never adopts it.
The production adapter regression covers reset arriving during task dispatch,
reconnect and fresh bytes in that same dispatch, and stale/current responses.

This guards the Pico Channel boundary, not shared FujiBus framing. IOService
polls once, then handles and responds to queued requests serially, so responses
to a slow request and the remaining queued requests retain the old read epoch.
Existing shared framer partial/queued state still lacks epoch fencing. For
example, an invalid frame can end a drain early, allowing a later poll to read
new-session bytes before an older queued frame is dispatched. Complete shared
framer/queue session isolation remains separate deferred work.
