# NIO on the Raspberry Pi Pico 2 W

A first port of NIO's firmware to the **Pico 2 W** (RP2350, Cortex-M33, CYW43439
Wi-Fi). The portable core (`src/lib`), config store, devices, Wi-Fi link, raw TCP/UDP/DNS
and TNFS backends build and link for the board. Remaining stubs are listed below. Nothing here
has run on hardware yet.

It follows the architecture rule: no `#ifdef` in shared code. The port is a new
`src/platform/pico/` plus a `main_pico.cpp`, selected by being a different
project, as `bridges/*` are.

## Build

```sh
source scripts/env.sh                      # exports PICO_SDK_PATH (AUR pico-sdk)
cmake -S pico2w -B build/pico2w
cmake --build build/pico2w -j
```

Needs `arm-none-eabi-gcc` and `arm-none-eabi-newlib`, and the Pico SDK with its
lwIP, mbedTLS and CYW43 submodules. The first configure downloads picotool.
The default build type is `MinSizeRel` (`-Os`). The result is
`build/pico2w/fujinet_nio_pico2w.uf2`.

## Size of the skeleton

Pico SDK 2.3.1, GCC 16.2, `-Os`, `--gc-sections`, exceptions and RTTI on (as on
ESP32), USB stdio:

| | KB |
|---|---|
| Flash (`.bin`) | 835 |
| Static RAM (bss + data) | 61 |

Flash by component: CYW43 driver 231 (almost all of it the radio firmware
blob), NIO `src/lib` 219, libstdc++ 183, newlib and libgcc 87, yaml-cpp 72,
lwIP 29, Pico SDK and TinyUSB 26, NIO platform 6. Static RAM is mostly lwIP's
pools (44).

**This is a lower bound, not NIO's footprint.** It leaves out everything the
stubs below leave out: TLS (mbedTLS is linked in only when something calls it),
the HTTP and TCP protocols, the filesystem, the USB channel and the console,
HotSync, and any runtime heap, packet buffers or stacks. The board has 520 KB of
SRAM and 4 MB of flash.

## What is real and what is a stub

| Area | State |
|---|---|
| Core, devices, config, services | Real (`src/lib`) |
| Build profile | `src/lib/build_profile/rp2350_pico2w.cpp` (FujiBus over USB CDC, not wired up) |
| Wi-Fi link | Real: `src/platform/pico/wifi_link.cpp`, CYW43 through `cyw43_arch`. No scan, no BSSID pinning |
| Sockets (TCP, UDP, DNS) | Raw lwIP: bounded receive queues, opaque addresses, TCP listener/client and TNFS TCP/UDP |
| TCP protocol | Portable TCP state machine over raw lwIP |
| TLS, HTTP | **Stub**: `network_registry.cpp` registers the stub protocol |
| Channel (USB CDC, UART) | **Stub**: `channel_factory.cpp` returns nullptr; `main_pico.cpp` runs without a transport |
| Filesystem | **Stub**: no `flash` filesystem, so config stays in memory |
| Console | **Stub** |
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

Plan: LittleFS on the on-board flash for `fujinet.yaml`, then SD over SPI. TNFS
and HTTP filesystems already exist in `src/lib` and need sockets.

### Memory

`lwipopts.h` and `mbedtls_config.h` are first guesses (a TLS 1.2 client with
ECDHE and a 4 KB record). RAM for TLS plus buffers is the open question.

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

## Next steps

1. Socket backend (lwIP raw or FreeRTOS), then TCP/UDP/DNS and TNFS.
2. USB CDC channel, then UART.
3. LittleFS on flash.
4. TLS and HTTP on mbedTLS; re-measure RAM.
5. Run on hardware.

## Ordered port measurements (build verification only)

GCC 16.2 / SDK 2.3.1, MinSizeRel. Bytes, not rounded KB. Flash is the generated
`.bin`; static RAM is `.data + .bss + .ram_vector_table + .uninitialized_data`.
`arm-none-eabi-size`'s default Berkeley columns classify Pico `.data` as text,
so use `arm-none-eabi-size -A` and the linker map instead.

| Stage | Flash bytes | Static RAM bytes | Reserved heap/stack bytes |
|---|---:|---:|---:|
| Baseline skeleton (remeasured) | 855364 | 68884 | 2048 / 2048 |
| 1: sockets and TNFS | 934792 | 146836 | 2048 / 2048 |

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
