# Packet link over a byte stream

A packet link carries complete raw FujiBus packets over a byte stream (a UART,
a TCP socket or a serial port) for `TransportKind::FujiBusNative`. It is the
stream profile of the [bridge packet ABI](../bridges/rp2350-zorro/docs/bridge-packet-abi.md):
the same one-outstanding exchange, explicit completions, generations and
no-replay rules, over wires that have no packet boundaries of their own.

It exists for boards where a microcontroller sits between a host bus and the
ESP32 and is joined to it by a UART, such as the Mac floppy-port board: an
RP2040 serves the Mac's floppy and HD20 signalling and turns what the Mac asks
for into ordinary DiskDevice and other FujiBus requests. NIO sees only FujiBus
packets; it knows nothing about the bus behind the controller.

Related documents:

- [Native packet contract](native-packet-contract.md): `IPacketIO`, `NativeFramer` and their outcomes.
- [Bridge packet ABI](../bridges/rp2350-zorro/docs/bridge-packet-abi.md): exchange, completion and generation rules.
- [Build profiles](build_profiles.md): selecting `FujiBusNative` over a stream channel.

## Roles

- The **controller** (the bridge microcontroller, an emulator, or a test tool)
  starts every exchange and owns generations and retries.
- The **peer** is NIO. It answers; it never sends anything unasked.

## Records

Every record, in both directions:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 2 | sync `F5 4E` |
| 2 | 1 | kind |
| 3 | 1 | generation |
| 4 | 2 | body length, little-endian |
| 6 | n | body |
| 6+n | 4 | CRC-32 (IEEE 802.3, as zlib), little-endian, over offsets 2 to 5+n |

| Kind | Direction | Body |
| --- | --- | --- |
| `0x01` Packet | both | one raw FujiBus packet (`serializeRaw()`) |
| `0x02` Error | peer to controller | one error code |
| `0x10` Sync | controller to peer | empty |
| `0x11` SyncAck | peer to controller | empty |

Error codes: `1` Corrupt (a record whose CRC failed), `2` Oversized (body over
the peer's capacity), `3` Busy (a request while one is outstanding), `4`
NotSynchronised (no Sync yet, or a different generation), `5` Empty.

A receiver scans for the sync bytes and ignores anything else. A record whose
length is over capacity, or whose CRC fails, is dropped and scanning resumes
just after its sync bytes. The peer reports it (Oversized or Corrupt) only if it
claims to be a Packet in the current generation while nothing is outstanding;
otherwise it is dropped silently and the controller's timeout covers it. Sync bytes may appear inside a body; the length and CRC,
not the absence of sync bytes, delimit a record. A record that stops arriving
for 100 ms is abandoned.

## Capacity

Each peer has a fixed raw-packet capacity, 4096 bytes by default, and rejects
larger bodies with Oversized. A controller must split larger work, for
example by reading a floppy track with more than one `ReadSectors`.

## Generations

1. The controller sends Sync with a new generation `g`, any time.
2. The peer discards every queued, partial and outstanding packet, adopts `g`
   and answers SyncAck `g`. A response still being prepared for an earlier
   generation is dropped when it is sent.
3. Only then may the controller send requests, each as a Packet in `g`.

The controller starts a new generation at startup, after a timeout, and after
any completion it cannot match to its request. A peer restart loses the
generation, so its next answer is NotSynchronised and the controller resyncs.

## Exchanges

One request is outstanding at a time:

1. the controller sends a Packet in the current generation;
2. the peer answers with exactly one completion in that generation: the
   response Packet, or an Error;
3. the controller takes that completion before sending another request.

An Error means the request was not executed; the controller may send it again
as a new request. A timeout is an unknown completion: the request may or may
not have run, so the controller starts a new generation and does not resend
it. Whether to retry at a higher level, for example returning a disk error to
the host, is the controller's decision.

## NIO

`io::PacketLink` implements `IPacketIO` on top of any byte `Channel`, and
`io::PacketLinkChannel` owns both and exposes the link as `packet_io()`. The
POSIX and ESP32 channel factories wrap a stream channel in a
`PacketLinkChannel` when the profile's transport is `FujiBusNative`; a Pty
channel stays unwrapped, as it is for the Zorro placeholder.
