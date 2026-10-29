# Packetized video transport (0.8.0)

Peers advertise `FeatureDatagramVideo` (524288) in the authenticated hello.
Both ends must advertise it. A new video connection exchanges message 35 with
its ephemeral UDP port over the existing encrypted TCP video lane. Port zero
declines UDP. Old peers receive no new message and retain their TCP contract.
The new lane is bound to that connection's ECDH transcript; it is not a discovery
service, an input lane, or a topology-specific exception.

## Packet and cryptographic boundary

- Maximum UDP payload is 1188 bytes, below 1280 bytes including IPv4/UDP headers.
  A video fragment contains at most 1100 bytes of the existing frame message.
- Header: `HDU`, version byte 1, big-endian 64-bit packet encryption counter.
  AES-256-GCM encrypts the body and authenticates that header; tag is 16 bytes.
- Direction-specific HMAC-SHA256 labels derive keys from the video pair root.
  TCP, opposite directions and different handshakes never share key/nonces.
  Retransmissions have fresh encryption counters and stable logical packet IDs.
- A bounded 65536-counter replay window permits reordered packets. It advances
  only after successful authentication. The authenticated TCP peer's IP/port is
the only permitted UDP endpoint.
- From 0.8.2, bind UDP to the TCP connection's actual local address. A wildcard
  socket on a multi-address host can otherwise send from a different source IP;
  relaxing authentication/endpoint checks would conceal that routing mistake.
- Bodies: Hello; Data (packet ID, reference epoch, frame ID, total bytes,
  offset, fragment); selective ACK (up to 128 packet IDs); reference Reset.
  Strict size/offset/identity checks precede allocation or copying.

## Scheduling and recovery

The sender does not wait for a whole-frame ACK before capturing another frame.
One unsent frame feeds a paced, bounded packet window across multiple frames.
The congestion window starts at 32 packets, grows on delivery when utilized,
and decreases on sustained RTT inflation or substantial repair traffic. Small
isolated losses use selective retransmission; they do not each halve the rate.
RTT is measured only on packets that were not retransmitted. From 0.8.4 its
baseline is the minimum over ten seconds of samples, not the connection's
lifetime minimum. Short queue growth still reduces the window; persistent path
changes eventually replace the old baseline without resetting congestion credit.
Timeout episodes exponentially back off the RTO (at most once per timeout
interval, capped at one second), until unambiguous ACKs supply a fresh RTT.
This prevents a longer path from making every new packet a retransmission and
therefore preventing RTT measurement indefinitely. The timeout has
room for a full RTT plus jitter, rather than mistaking every queue fluctuation
for loss. This is a custom controller, not an implementation of BBR or WebRTC.

High-resolution scheduling permits at most 1 ms of catch-up; it cannot emit a
seconds-long burst after a delayed thread wake. Control/input retain independent
TCP lanes. ACKs and reference resets precede new video packets. The window is
bounded to 4096 packets, 128 retained frames and 32 MiB of payload ownership.
Cancelling a session releases its payload but preserves outstanding congestion
credits until ACK/timeout. Repeated keyframes cannot bypass the window.

An IDR begins a monotonically increasing reference epoch. Every fragment carries
it, so reordered P-frames cannot escape ahead of their IDR. Complete frames are
released in reference order to the existing bounded decoder queue; presentation
still uses the latest ready image. Late epochs are acknowledged but never shown.
Session identity remains in the original authenticated frame message.

From 0.8.1, complete frames wait for decoder queue credit without blocking UDP
receive/ACK work. Their real final-fragment timestamp is retained while waiting
for references or credit. Both incomplete and completed retained frames count
towards the same 32 MiB / 128-frame receive budget. Expired reference ownership
requires a new IDR, with bounded repeat reset requests until a new epoch arrives.

UDP establishment has a 3 s limit. A retained frame has a 2 s transport lifetime.
A blocked sender switches to TCP and requires an IDR, without disconnecting the
independent reciprocal stream. Send and receive expiration counters are separate;
only an outgoing frame expiry triggers local sender fallback. Receiving the first
TCP frame fences late UDP pictures on that direction. Stop wakes the worker, joins it outside the
connection-map lock and releases the socket before final Winsock cleanup.

### Recovery on the same connection (0.8.4)

`FeatureDatagramRecovery` (1048576) must be advertised by both peers. Older peers
retain their previous one-way fallback behavior. While using TCP, authenticated
UDP challenge/reply packets prove round-trip reachability. Recovery requires
three fresh replies after a five-second cooldown; failed attempts back off up to
sixty seconds. Thirty seconds of resumed UDP with actual ACK progress resets
that backoff. Small probes establish reachability, not bulk capacity.

After successful probes, new video admission pauses and message 36 carries a
unique reference epoch on the existing encrypted TCP video lane. It follows
already-submitted TCP frames, replaces only the unsent tail, and cannot expire
under the picture's 50 ms freshness rule. The receiver processes that barrier
after prior TCP frames, discards older UDP epochs, and echoes the permitted epoch
in authenticated probe replies. Only that acknowledgement permits a fresh UDP
IDR; subsequent P-frames cannot depend on the old TCP chain. Each direction has
its own handoff and the same control/input connections remain active.

An unacknowledged barrier times out after three seconds and restores TCP with a
new IDR. Late replies cannot activate an abandoned attempt; late barriers remain
ordered before subsequent TCP frames, which fence UDP again. No graphics pipeline
or source resolution/quality setting is changed by recovery.

Unique authenticated fragment bytes update receiver progress, including an
incomplete IDR. Retransmitted duplicates do not inflate measured goodput.
`tcp_diagnostics()` now also reports selected media lane, packets, retransmits,
RTT, congestion window, in-flight ownership and expirations. Its TCP counters
continue describing the TCP sockets, not the UDP payload rate.
Local telemetry also records these as separate `packet_*` fields;
`packet_transport` is 0 absent, 1 negotiating, 2 UDP, 3 TCP fallback, 4 recovering.
`packet_send_expired` and `packet_receive_expired` distinguish the failing
direction; `packet_expired` remains their total for existing consumers. TCP write
timings must not be interpreted as UDP send latency.

## Validation and limits

`HiveDeskDatagramCheck` exercises deterministic capacity/RTT/loss simulations,
ordered reassembly, missing references, duplicate packets, bounds, cancellation,
epoch replacement, rapid-session congestion accounting, authenticated crypto,
real full-duplex loopback, a midstream black hole, TCP fencing and bounded stop.
The existing transport suite retains explicit TCP/mixed-capability checks.
Hidden hardware loopback and real-peer measurements are separate gates.

There is no FEC in this version. A missing predictive fragment can still delay
its dependent pictures by a retransmission round trip. A zero-loss generated
path test is not proof of motion smoothness, and simulated time is not a
benchmark of actual Windows scheduler, GPU, radio or remote-host performance.
Codec, native resolution, HEVC 4:4:4, QP policy and physical refresh are unchanged.

Design references: [IETF UDP usage guidance](https://www.rfc-editor.org/rfc/rfc8085)
and [WebRTC paced sending](https://chromium.googlesource.com/external/webrtc/+/master/modules/pacing/g3doc/index.md).
These explain why congestion control, packet size, reliability and burst pacing
must be handled together; they do not certify this implementation.
