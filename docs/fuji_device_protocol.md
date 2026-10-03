# FujiDevice Binary Protocol

FujiDevice (`WireDeviceId::FujiNet`, currently `0x70`) provides small,
instance-level control operations. Persistent slot choices are owned by
SlotCatalogService, while active disk mappings are owned by DiskDevice.

See:

- [Slot catalogue and active disk mounts](slot_state.md)
- [FileDevice protocol](file_device_protocol.md)
- [Slot Catalog Service protocol](slot_catalog_service_protocol.md)
- [DiskDevice protocol](disk_device_protocol.md)

## Commands

| Command | ID | Purpose |
|--------:|---:|---------|
| `GetInfo` | `0x01` | Report the firmware version and build profile |
| `Reset`   | `0xFF` | Request a FujiNet reset/restart |
| `GetSsid` | `0xFE` | Reserved; not currently implemented |

## GetInfo (`0x01`)

Describes this FujiNet. It needs no network or other device, so it answers
quickly on any board.

Request payload: version `u8`, the highest version the client understands
(currently `1`). An empty payload or version `0` is `InvalidRequest`; bytes
after the version are ignored.

Response payload:

| Field | Type | Notes |
| --- | --- | --- |
| version | `u8` | the lower of the requested version and the firmware's; `1` today |
| firmware version | `u8` length + text | at most 32 bytes, e.g. `0.1.1` |
| build profile | `u8` length + text | at most 64 bytes, e.g. `S3 + FujiBus over GPIO (e.g. RS232)` |

This command follows the
[extending-commands rules](protocol_reference.md#extending-commands): a
version-1 reply always has exactly these fields, new fields come with a new
version, and a client asking for a newer version than the firmware knows gets
the newest reply the firmware can give, marked with its version.
Firmware without this command answers `Unsupported`. The station MAC address
comes from the Wi-Fi service's `GET_ADAPTER_INFO`
([Wi-Fi service protocol](wifi_service_protocol.md)). On the console, `core.info`
shows the same version and profile; from a host, `./scripts/fujinet --port <port>
fuji info` asks for it.

## Reset (`0xFF`)

The request and successful response have no payload.

`IOResponse.status` is returned through the FujiBus status metadata:

- `Ok`: reset accepted.
- `Unsupported`: no reset handler is available or the command is unknown.

On an MCU the reset handler may restart the device before returning a response.

FujiDevice does not expose slot catalogue or disk mount commands. Clients use
SlotCatalogService for catalogue entries and DiskDevice `Mount`, `Unmount`, and
`ListMounts` for active disk units.
