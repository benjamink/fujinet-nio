# NIO on the Raspberry Pi Pico 2 W

A first port of NIO's firmware to the **Pico 2 W** (RP2350, Cortex-M33, CYW43439
Wi-Fi). The portable core (`src/lib`), config store, devices, Wi-Fi, raw TCP/UDP/DNS,
TNFS, USB CDC, UART console, LittleFS, TLS and HTTP/HTTPS build and link for the
board. It was first brought up on a real Pico 2 W on 2026-11-16: what that verified, and what
is still untested, is under "Hardware acceptance" below.

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

CI (`.github/workflows/pico2w.yml`) builds the firmware and runs every native test project below on each
pull request, with the Pico SDK 2.3.1 and Ubuntu 24.04's `gcc-arm-none-eabi` 13.2. Sizes in this
document were measured with GCC 16.2; GCC 13.2 produces a larger image for the same source
(1235396 bytes against 1176924 at the time of writing), so compare sizes across the same compiler.
`FN_LOG*` compile to nothing unless `FN_DEBUG` is defined, as on the other targets, so
the default image is silent on UART0. Configure with `-DFN_DEBUG=ON` to get the boot
banner and logs on UART0 (GPIO0 TX / GPIO1 RX, 115200 baud); it adds about 19.5 KB.

**Wi-Fi power save is off** (`NIO_PICO_WIFI_PM=none`). The CYW43 driver's own default sleeps the
radio between beacons, which on hardware added 70 to 230 ms to every round trip (ping averaged
150 ms, against 1.3 ms with it off). The adapter is USB powered, so the cost is irrelevant;
configure with `-DNIO_PICO_WIFI_PM=default`, `performance` or `aggressive` for a special case.

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
the implemented network backends. An SD card on SPI is the filesystem `sd0` (below).

### SD card (`sd0`)

A FAT-formatted SD/SDHC/SDXC card wired to SPI0 appears as `sd0:/`, with long names and
UTF-8 file names. FAT12/16/32 only: exFAT (cards over 32 GB as sold) is not supported, so
format larger cards as FAT32, or use the console command `format sd0 confirm`, which
creates FAT32 on the whole card and destroys what is on it.

| SD module pin | Pico 2 W pin | GPIO |
|---|---|---|
| CS (SS) | 22 | GP17 |
| SCK (CLK) | 24 | GP18 |
| MOSI (DI) | 25 | GP19 |
| MISO (DO) | 21 | GP16 |
| GND | 23 (or any GND) | |
| VCC | 36 (3V3 OUT) | |

A bare SD breakout or socket takes 3.3 V. Modules with an onboard regulator and level
shifter (usually marked "5V") are normally fed 5 V from VBUS (pin 40); their level shifters
work with 3.3 V signals. Keep the wires short: the default 10 MHz clock is for breadboard
wiring (cards allow 25 MHz, `NIO_PICO_SD_BAUD`). The pins are CMake cache variables
(`NIO_PICO_SD_SPI`, `_MISO`, `_CS`, `_SCK`, `_MOSI`); any pins valid for the chosen SPI block work.

- The card is looked for once, at boot. With no usable card there is no `sd0` and nothing
  changes. A card inserted later needs a restart. A card that was mounted and then errors is
  re-initialised on the next access (at most once a second); files opened before are then dead.
- As on ESP32, a present `sd0` becomes the default persistent store for disk images and the
  app store. `fujinet.yaml` stays on flash.
- The driver (`src/platform/pico/sd_card.cpp`) speaks SPI mode directly. FatFs R0.15 comes from
  the Pico SDK's TinyUSB (`lib/tinyusb/lib/fatfs`) and is built with `pico2w/fatfs/ffconf.h`,
  so the SDK's TinyUSB submodules must be fetched. Core 0 only: FatFs is not built reentrant.
- Native tests (`tests/pico-sd`) run the real driver and the real FatFs against a simulated
  card that enforces the SPI-mode protocol, and check the volume FatFs writes with `fsck.vfat`
  when it is installed. They need `PICO_SDK_PATH`.

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

## Hardware acceptance

Brought up on one Pico 2 W on 2026-11-16. Early bring-up used `FN_DEBUG` images for the UART logs;
every performance figure below is from the release build, because debug builds are about 100 times slower.

**Verified on the board**

- Boot, the dual-core split (core 1 USB, core 0 everything else), USB CDC enumeration and
  FujiBus over it, and the UART0 console including `format`.
- LittleFS formatting, persistence of `fujinet.yaml` across reboots, and recovery of a board
  whose last flash page was not blank. A 3 KB app-store write (a flash commit) takes about 50 ms
  with a worst case of 100 ms, which is how long USB pauses; readback was identical.
- Wi-Fi join with credentials set from the console and kept across reboots, DHCP and DNS
  (1.3 ms ping with power save off, 150 ms with the driver default).
- TCP and HTTP: 64 KiB transfers byte-exact (about 210 KB/s over TCP, 90 to 160 KB/s over HTTP).
  POST, PUT and DELETE with bodies up to 12 KB echo back byte-identical; redirects work
  (same-origin and HTTP to HTTPS, at most 3, HTTPS to HTTP refused) when the request sets the
  follow-redirects flag.
- HTTPS: 13 real sites complete a handshake and return an HTTP status (Let's Encrypt, Google,
  GitHub and Gist, Cloudflare, Amazon, Microsoft, Wikipedia, Mozilla, Apple, python.org,
  kernel.org and example.com), as does the valid badssl.com control; badssl's untrusted-root,
  wrong-host, expired and self-signed hosts are refused.
- Large HTTPS downloads: 20 consecutive 2 MB files and 5 of 256 KB from a local server, every
  checksum correct, at about 119 KB/s; github.com's 525 KB home page in about 8 s.
- TNFS over UDP against a LAN tnfsd: reads byte-exact for chunk sizes 512 to 4096 (about
  60 KB/s with the read-ahead held to the 4-datagram queue), and a 100 KB write read back identical.
- Memory high-water marks after the 40 MB TLS soak (`pico.mem` on the console): core 0 stack
  5.2 of 16 KB, core 1 stack 0.4 of 4 KB, lwIP heap (all TLS allocations) 44 of 128 KB with no
  failed allocation, newlib heap arena 88 of 158 KB.

**Not yet verified on hardware**

- Wi-Fi drop and rejoin, and USB unplug and replug under traffic; a soak of hours.
- Power-loss recovery of flash.
- TLS 1.3-only servers (the build speaks TLS 1.2), SD over SPI (not implemented), and the
  optional UART transport, scan/BSSID selection and SNTP (not added).

**Known limitation: HTTPS handshakes with elliptic-curve certificates are slow.** RSA-chained
sites take 1.1 to 1.9 s, P-256 chains about 5.4 s and P-384 chains 7.7 to 9.0 s, because mbedTLS
does the curve arithmetic in software at 150 MHz (they were 20% slower before mbedTLS's arithmetic
was built `-O2`; see `NIO_PICO_MBEDTLS_OPT`). Resuming a session would skip the handshake for
repeat connections to a host; it is not implemented.

**Stress-testing TLS locally.** Public hosts rate-limit and drop connections, which makes a flaky
download impossible to tell from a bug. Build with `-DFN_HTTPS_TEST_CA_ADDITIVE=ON` (trust the
FujiNet test CA as well), run `scripts/start_test_services.sh http-fs`, put large files in
`/tmp/fujinet-http-fs`, and fetch them with
`./scripts/fujinet -p <port> net get --chunk 4096 --out f https://<host-ip>:18443/<file>` after
`./scripts/fujinet -p <port> clock set`, comparing checksums. This is how the two stalls described
under "TLS receive" were found; they failed 6 of 6 downloads before the fixes.

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
ownership until closed. A partition is formatted automatically only when both superblock
copies (blocks 0 and 1) are erased; a volume that ever existed has data there, so damage in
either copy leaves the filesystem unavailable and is never silently reformatted. Blocks
outside that pair are not judged, as LittleFS erases a block before it writes it. (The first
Pico 2 W brought up on hardware had a single factory-written 0xEF page at the very end of
flash, 0x103fff00. Judging the whole MiB, as this code first did, turned that into
`Corrupted dir pair at {0x0, 0x1}` followed by `existing nonblank storage was not
formatted`, and the config stayed in memory only.) Read failure or format failure also
leaves the filesystem unavailable.

For a volume with a damaged superblock, the operator's way out is the console command
`format flash confirm`: it recreates an empty LittleFS superblock and reboots. It does not
wipe the other blocks, so it is not a secure erase. Without `confirm` it only prints a usage
line and a warning.

The native `tests/pico` target uses the real LittleFS C implementation over bounded
RAM flash. Cases cover persistence across remount, handles outliving filesystem
objects, directory/file operations, damage in either superblock copy (refused), stray
bytes elsewhere (formatted), explicit format, I/O failures, and checked page/block
boundaries. Firmware links with the partition
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

**TLS receive.** The SDK adapter decrypts into pbufs and hands the application everything it has
queued as one chain; a chain the application refuses (`ERR_MEM`) stays queued and is retried only on
a poll tick or the next network packet. Two things made large downloads stall until the 15 s
inactivity timeout, every time on a 2 MB file:

- No poll callback was registered, and the adapter arms its poll tick only if the application has
  one, so data refused at the end of a stream was never redelivered and the transfer stopped a few
  hundred bytes short. `altcp_poll` is now registered (its body is empty; registering it is what
  matters).
- Refusing was itself the bug. With a 20 KiB TCP window the queued chain can be longer than the
  16 KiB ring, so it could never be accepted even into an empty ring. The adapter now takes every
  chain, moves what fits into the ring, keeps the rest and drains it as the application reads (the
  credit for each byte is still returned as it is read, so the window behaves as before). Bytes
  received before an error stay readable.

Platform trust is the **Mozilla root store** (114 roots, 121 KB of flash), packed by
`pico2w/tools/make_ca_bundle.py` from `pico2w/certs/mozilla-roots.pem` (curl's published extract:
source, date and hash in `pico2w/certs/README.md`). The device does not parse the set: mbedTLS is
given a callback (`MBEDTLS_X509_TRUSTED_CERTIFICATE_CALLBACK`) that, while verifying a chain,
looks the issuer's name up in an index sorted by a hash of each subject and parses only the
matching root (`detail/ca_bundle.h`, `tls_trust.cpp`). Verification adds 6.6 to 13.9 KB of heap per
handshake in native measurements, not the 280 KB parsing 114 roots would. An invalid bundle makes
TLS open fail; nothing is ever trusted by default. The generator drops roots this mbedTLS cannot
use, and reports why: signatures with SHA-1 or MD5, RSASSA-PSS, EC curves other than P-256/P-384,
RSA above 4096 bits, and roots expired at the snapshot date (4 of 118 in the current set).

Trust matching is by exact subject bytes, as the issuer field of a chain element names it. A
CA that re-encodes its own name differently between a root and the certificates it signs would
not be found; none of the twelve major sites tested is affected.

`-DNIO_PICO_CA_BUNDLE=/absolute/path/roots.pem` (and `-DNIO_PICO_CA_DATE=YYYY-MM-DD`, its
snapshot date) substitute another PEM. `-DFN_HTTPS_TEST_CA=ON` trusts only the existing FujiNet
test CA; `-DFN_HTTPS_TEST_CA_ADDITIVE=ON` adds it to the platform roots. These options are
mutually exclusive. There is no verification-disabled fallback.

The firmware config enables `MBEDTLS_SHA384_C`. It is separate from SHA-512 in mbedTLS 3, and
without it every SHA-384-signed certificate fails to parse. Most current intermediates (Google
WE1/WE2, Sectigo, SSL.com, Microsoft, ISRG Root YE) are, so no HTTPS site that uses them can be
verified however good the trust store is. The native tests pin this: they parse every
certificate of twelve real server chains and verify them.

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
are bounded to 8 KiB and URLs to 2048 bytes. Response metadata (the status line, headers
and trailers together) is bounded to 16 KiB, a single response header line to 8 KiB,
chunk-size lines to 128 bytes, and header counts to 64. The response limits are generous
on purpose: github.com sends a 4.4 KB Content-Security-Policy on one line (5.8 KB of
headers), and a 2 KiB line limit turned that valid response into an IOError.
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
- `tests/pico-sd` → `/tmp/nio-pico-sd-tests`: the SD driver and FatFs against a simulated card (needs `PICO_SDK_PATH`).
- `tests/pico-http` → `/tmp/nio-pico-http-tests`: 14 cases / 4851 assertions covering
  parser framing, malformed responses, methods/uploads/replay, redirects,
  timeout and a 20 KiB response across queue backpressure.
- `tests/pico-tls` → `/tmp/nio-pico-tls-tests`: 3 cases / 1342 assertions using the
  actual SDK mbedTLS and firmware configuration for trusted/wrong-host/unknown-CA/
  future/expired certificates, entropy boundaries and Pico clock progression.
- `tests/pico-tls-adapter` → `/tmp/nio-pico-tls-adapter-tests`: three builds test
  default/test-only/additive trust selection (which sources the connection is anchored in),
  hostname setup, fail-closed verification/time, callback detachment, partial writes, limits and
  timeout, plus receive credit/refusal/replay/EOF/error: 5 cases per build. Three more builds
  use a deliberately invalid bundle and check that TLS refuses to connect.
- `tests/pico-ca` → `/tmp/nio-pico-ca-tests`: the trusted-root bundle. One target tests the
  format and lookup with synthetic bundles (malformed, cross-signed, a constructed hash
  collision, every single-byte corruption). The other builds the SDK's real mbedTLS with the
  firmware configuration and runs the production lookup against the full Mozilla bundle:
  every bundled root parses with the subject the index says, twelve real server chains (Let's
  Encrypt, Google, GitHub, Cloudflare, Amazon, Microsoft, Wikipedia, Mozilla, Apple, python.org,
  kernel.org, example.com) verify, and untrusted-root, wrong-hostname, expired, not-yet-valid,
  tampered-signature, missing-intermediate and no-trust cases are refused, with a counting
  allocator checking nothing leaks, including under injected allocation failures at every
  point. The chains were captured on 2026-11-16 and are verified at that date.
- `tests/test_pico_ca_bundle.py` (`python3 -m unittest discover -s tests -p 'test_pico_*.py'`):
  the bundle generator's filtering, layout and determinism.

For example, `cmake -S tests/pico-http -B /tmp/nio-pico-http-tests`, then
`cmake --build /tmp/nio-pico-http-tests -j8` and
`ctest --test-dir /tmp/nio-pico-http-tests --output-on-failure`.
These tests exercise native logic and mocked hardware boundaries; they are not
USB/radio/flash hardware or live-TLS peak measurements. Raw and LittleFS targets
also pass AddressSanitizer/UndefinedBehaviorSanitizer (including vendor C).

### A flaky test, found and fixed

`fujinet-nio-tests` failed now and then on CI and once on a developer machine (2026-11-16), always
at `tests/test_network_device_tcp.cpp` in `TCP: Read preserves bytes across 64K stream and ring
boundary` (CI runs on Oct 30, Nov 1 and Nov 16). An earlier note here blamed the image-translator
stack check; that was a wrong guess.

The test's ring is exactly full at the end of its stream, so its final read of the 14-byte marker
could return a single byte, and the test then asked again at the same offset. That is a retry of the
last read and is answered by replaying it, so the loop never advanced and ran out its 1.5 s deadline.
It showed up only when the rest of the marker arrived late (Nagle's algorithm and the delayed ACK
make that about 40 ms on an idle machine), reproduced about 1 run in 600 under heavy CPU load, and
is a bug in the test, not in the product: a client continues at the offset where its last read ended.
The test now does. It ran 6000 times under the same load with no failure, and a thread-free test
pins the protocol rule it had broken.

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
- [x] Physical USB, Wi-Fi, flash, TCP/HTTP/HTTPS and TNFS-read acceptance (2026-11-16; see "Hardware acceptance").
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
