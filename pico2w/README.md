# NIO on the Raspberry Pi Pico 2 W

A first port of NIO's firmware to the **Pico 2 W** (RP2350, Cortex-M33, CYW43439
Wi-Fi). It is a skeleton: the portable core (`src/lib`), the config store, the
devices, the Wi-Fi link and the tick loop build and link for the board, and
everything that needs a real platform backend is a documented stub. Nothing here
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
| Sockets (TCP, UDP, DNS) | **Stub**: `socket_ops_unavailable.cpp`, every call fails with `ENOSYS` |
| TLS, HTTP | **Stub**: `network_registry.cpp` registers the stub protocol |
| Channel (USB CDC, UART) | **Stub**: `channel_factory.cpp` returns nullptr; `main_pico.cpp` runs without a transport |
| Filesystem | **Stub**: no `flash` filesystem, so config stays in memory |
| Console | **Stub** |
| Memory stats | Approximate (newlib `mallinfo`) |
| Threads | Shims: `compat/mutex` is a real Pico SDK mutex; `compat/condition_variable` polls; no service tasks |
| Time, logging | Shared with `src/platform/posix` (portable newlib code) |

### Sockets

The Pico SDK's usual lwIP setup (`NO_SYS=1`, `pico_cyw43_arch_lwip_*_background`)
has no BSD socket API. The real backend is either lwIP raw pcbs behind
`ITcpSocketOps` and `IUdpSocketOps`, or FreeRTOS with lwIP sockets. That choice
should be made before writing the TLS and HTTP backends.

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
| `src/lib/udp_channel.cpp` | Needs BSD socket types; comes back with the socket backend |
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
