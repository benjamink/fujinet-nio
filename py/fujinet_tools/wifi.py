from __future__ import annotations

from typing import Optional

from .common import open_serial, status_ok
from .fujibus import FujiBusSession

# -----------------------
# Wi-Fi service commands (docs/wifi_service_protocol.md)
# -----------------------
WIFI_SERVICE_ID = 0xF3
WIFI_CMD_GET_ADAPTER_INFO = 0x05
WIFI_VERSION = 1


def build_get_adapter_info_req() -> bytes:
    return bytes([WIFI_VERSION])


def parse_get_adapter_info_resp(payload: bytes) -> Optional[str]:
    """version, MAC-present (u8), six MAC bytes. Returns "aa:bb:cc:dd:ee:ff",
    or None when the adapter has no MAC to report."""
    if len(payload) < 8 or payload[0] != WIFI_VERSION:
        raise ValueError("bad GET_ADAPTER_INFO response")
    if not payload[1]:
        return None
    return ":".join(f"{b:02x}" for b in payload[2:8])


def cmd_wifi_adapter(args) -> int:
    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)
        pkt = bus.send_command_expect(
            device=WIFI_SERVICE_ID,
            command=WIFI_CMD_GET_ADAPTER_INFO,
            payload=build_get_adapter_info_req(),
            expect_device=WIFI_SERVICE_ID,
            expect_command=WIFI_CMD_GET_ADAPTER_INFO,
            timeout=args.timeout,
            cmd_txt="WIFI_GET_ADAPTER_INFO",
        )
        if pkt is None:
            print("No response")
            return 2
        if not status_ok(pkt):
            print(f"Device status={pkt.params[0] if pkt.params else '??'} (older firmware answers Unsupported)")
            return 1
        try:
            mac = parse_get_adapter_info_resp(pkt.payload)
        except ValueError as e:
            print(f"Bad response: {e}")
            return 1
        print(f"mac: {mac or 'unavailable'}")
    return 0


def register_subcommands(subparsers) -> None:
    """Register Wi-Fi service commands under `wifi`."""
    pw = subparsers.add_parser("wifi", help="Wi-Fi service commands")
    wsub = pw.add_subparsers(dest="wifi_cmd", required=True)
    pwa = wsub.add_parser(
        "adapter",
        help="Show the Wi-Fi adapter's station MAC address",
        description="Ask the Wi-Fi service GET_ADAPTER_INFO for the station MAC address.",
    )
    pwa.set_defaults(fn=cmd_wifi_adapter)
