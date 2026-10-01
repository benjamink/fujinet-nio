# Packet link over a byte stream

A packet link carries complete raw FujiBus packets over a byte stream (a UART,
a TCP socket or a serial port) for `TransportKind::FujiBusNative`. It is a
proposed stream profile of the [bridge packet ABI](../bridges/rp2350-zorro/docs/bridge-packet-abi.md):
the same one-outstanding exchange, explicit completions, generations and
no-replay rules, over wires that have no packet boundaries of their own.

**Status:** link version 1, implemented by NIO and the Python tools. The bridge
ABI is still a review draft; this profile proposes concrete answers to two of
its open decisions, capacity (C1, reported by the peer in SyncAck) and the
fresh-generation handshake (C2, Sync/SyncAck), and may change with them.

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
| `0x10` Sync | controller to peer | `version` (u8): the highest link version the controller speaks |
| `0x11` SyncAck | peer to controller | `version` (u8): the version both now use; `capacity` (u16, little-endian): the peer's raw-packet capacity |

Later versions may add fields at the end of a Sync or SyncAck body; a receiver
ignores bytes it does not know, and ignores records of a kind it does not know.

| Code | Error | Meaning | Ran? |
| --- | --- | --- | --- |
| `1` | Corrupt | a record whose CRC failed | no |
| `2` | Oversized | a request body over the peer's capacity | no |
| `3` | (reserved) | formerly Busy; never sent | |
| `4` | NotSynchronised | no Sync yet, or a different generation | no |
| `5` | Empty | an empty request body | no |
| `6` | Unanswered | NIO took the request but gave no answer (below) | maybe |
| `7` | UnsupportedVersion | a Sync without a version the peer speaks | (Sync) |

A receiver scans for the sync bytes and ignores anything else. A record whose
length is over capacity, or whose CRC fails, is dropped and scanning resumes
just after its sync bytes. The peer reports it (Oversized or Corrupt) only if it
claims to be a Packet in the current generation while nothing is outstanding;
otherwise it is dropped silently and the controller's timeout covers it. Sync
bytes may appear inside a body; the length and CRC, not the absence of sync
bytes, delimit a record.

A record is abandoned when its bytes stop for 100 ms. The limit is on the gap
between bytes, not on the whole record, so a large record on a slow UART still
arrives. Because a record cut off part way (a controller restarting
mid-record, say) would otherwise read its length from whatever follows, a
controller stays quiet for at least 100 ms before every Sync. The peer
recognises that gap even if it was blocked waiting for input throughout.

## Capacity

Each peer has a fixed raw-packet capacity, 4096 bytes by default, reported in
SyncAck. It rejects larger request bodies with Oversized. A controller must
keep requests within it and must not ask for answers larger than it, splitting
larger work: for example reading a floppy track with more than one
`ReadSectors`, or a file with several reads no larger than the capacity.

## Generations

1. After a quiet pause, the controller sends Sync with a new generation `g`, any time.
2. The peer completes an outstanding request with Unanswered in its old
   generation, discards every queued and partial packet, adopts `g` and
   answers SyncAck `g` with the agreed version and its capacity. If the Sync
   has no version the peer speaks, it answers Error UnsupportedVersion in `g`
   and stays unsynchronised.
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

An Error other than Unanswered means the request was not executed; the
controller may send it again as a new request. Unanswered and a timeout are
unknown completions: the request may or may not have run, so the controller
starts a new generation and does not resend it. Whether to retry at a higher
level, for example returning a disk error to the host, is the controller's
decision.

NIO answers each request before it reads the next one: `IOService` handles a
request and sends its response in the same pass. The link relies on that. A
request still outstanding when NIO next reads from the link got no answer, so
the link completes it with Unanswered at once rather than leaving the controller
to time out. That happens when the body is not a valid FujiBus packet, or when
its answer cannot be sent (larger than the capacity, or than a raw FujiBus
packet can be). The request may have run in the second case, which is why
Unanswered is an unknown completion. The same rule means a late answer can
never complete a request from a later generation. If NIO ever handles requests
asynchronously, the link needs a different way to tie answers to requests.

## NIO

`io::PacketLink` implements `IPacketIO` on top of any byte `Channel`, and
`io::PacketLinkChannel` owns both and exposes the link as `packet_io()`. A
build profile asks for a link with `packetLink = true` (with
`TransportKind::FujiBusNative`); the POSIX and ESP32 channel factories then wrap
its stream channel (TCP or serial on POSIX, UART or USB CDC on ESP32) through
`io::with_packet_link`. Profiles that don't ask, such as the Zorro placeholder
over a Pty, are unchanged.
