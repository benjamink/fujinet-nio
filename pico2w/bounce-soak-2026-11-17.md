# Bouncy World Pico soak: 2026-11-17

Diagnostic firmware: 3764fe0c, FN_DEBUG off. The client failed after about
36 minutes with fn_write TIMEOUT, distinct from the earlier five-second
application reply deadline.

## Evidence

Capture: /tmp/bounce-pico.pcap, SHA256
defecd852ef03e1b947f6b9ec384cabd060eecf77c2435343addb49bd7d239a3.
4094 packets, one TCP conversation: 192.168.1.67:61301 (Pico) to
192.168.1.100:9003 (server). Packet timestamps span 12:12:02.728580 to
12:14:58.554590 UTC. A pcap records packet times, not when tcpdump was stopped;
the final packet time alone does not establish the end of the observation window.

Final exchange:
- Frame 4092, 12:14:58.422447: six request bytes, raw sequence 131526.
- Frame 4093, 12:14:58.422512: 149 reply bytes, acknowledges 131532.
  Server-side turnaround was 65 microseconds.
- Frame 4094, 12:14:58.554590: Pico ACK of all reply bytes (660520063).

Client screenshot: 20833 attempts, 20832 successful writes and frames; failed
fn_write took 1001 ms, reported TIMEOUT and zero of six bytes confirmed.
No client reply deadline fired. Two NIO timeouts are counted, but this evidence
alone does not identify both calls. The displayed zero response length is not a
measurement of zero raw USB bytes: fn_write records response length only when
the transport exchange succeeds.

Pico EOF report:
tx=125022 ack=125022 rx=3103134 read=3103134, ring=0,
unsent=0 unacked=0, seq=131532/131532, out=0/0 cb=0 err=0,
wait=30003ms. Both TX/RX totals exactly match the client's successful totals.
Thus the failed six-byte request was not accepted by the instrumented raw TCP
send path before this EOF. The previous reply was consumed from the Pico ring.

Server logs: TCP idle-read timeout at 12:15:28.422, precisely 30 seconds after
the last captured request. World heartbeat eviction followed at 12:15:40.850.
The EOF is consistent with server idle closure, not the initiating failure.
Pico reporting this event shows foreground/lwIP execution had not permanently
stopped. Kernel journal query for 12:14:45–12:15:45 returned no entries; this
does not establish that every possible USB fault would have been logged.

## Earlier recoverable loss

Frames 1248–1252 show a separate recoverable episode:
server reply at 12:12:55.850521 retransmitted at 12:12:56.134902;
Pico ACK at 12:12:56.145657; a six-byte Pico request at 12:12:57.303349
fills a sequence gap, then the server responds in 121 microseconds.
The first transmission of that request is absent from this server-side capture.
This is consistent with delivery loss/delay, but capture loss cannot be excluded
from a classic pcap without drop statistics. It does not explain the final
host-to-Pico write timeout.

## Next boundary to observe

Prioritize the host-to-Pico FujiBus/USB request path, firmware framing/dispatch,
and the response path. TCP evidence alone cannot distinguish these. Capture USB
with usbmon alongside server TCP, or add bounded counters for CDC bytes,
decoded requests, dispatched writes, queued replies and framing/bridge faults.
Keep verbose payload logging off and keep timeouts unchanged while obtaining
this evidence. Do not label this a confirmed CYW43, USB, or client bug yet.

Verification: tshark conversation, selected-frame and analysis-flag views;
capinfos metadata and digest; source inspection of fn_write diagnostic handling.
No runtime code changed for this analysis.
