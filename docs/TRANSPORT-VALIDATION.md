# Transport and reference validation

The rendering policy distinguishes valid late reference pictures from missing
references. Decode admission has bounded catch-up; conversion/presentation has a
separate local deadline. The one-second severe-backlog bound is a rejection
ceiling, never an intentional playback delay. A ready picture still replaces the
previous ready picture instead of accumulating a playback queue.

`HiveDeskPathCheck <IPv4> [tcp|udp] [Mbps 0.1-20] [seconds 1-10]` connects using the
normal automatic LAN handshake. Both peers must advertise the diagnostic
capability. It explicitly requests generated, incompressible traffic from the
peer; no desktop capture or remote-control authority is requested. The responder
limits concurrent probes to two, duration to ten seconds and rate to 20 Mbps.
Nothing runs automatically. Run measurements sequentially on otherwise idle
peers, without SMB copies or GPU test suites competing for resources.

The TCP measurement uses small independent payloads on the existing encrypted
video connection, including its current send admission. UDP uses 1200-byte
payloads with per-probe HMAC, sequence deduplication and authenticated endpoint
exchange on the control channel. Both use a high-resolution pacing timer.
A UDP result tests reachability and paced delivery, not HEVC loss recovery.
TCP's missing count can include an expired application packet; it is not a
measurement of IP packet loss. UDP missing counts are taken after a bounded
two-second drain. A sender scheduling shortfall must be distinguished from lost
packets by comparing offered/sent/received counts. Zero received UDP packets do
not prove inadequate bandwidth; endpoint/firewall delivery may have failed.

`HiveDeskRemoteVideoCheck <IPv4> [milliseconds 1000-120000]` opens a small
nonactivating read-only live preview and prints receive/presentation counts,
first-image timing, source diagnostics and selected packet assembly durations.
It requests no input authority or clipboard handoff. Completion with a picture
is not a performance pass. Static desktop activity is not a max-FPS benchmark.

Validation gates: native CTest (including actual loopback TCP/UDP probe and
cancellation), managed/WinUI checks, isolated GPU presentation and source
freshness checks, then sequential real-peer measurements. Never substitute a
successful watchdog recovery or a green local queue metric for fresh-picture
throughput and frame-gap measurements.

## Live observations, 2026-10-28

After 0.8.1 OTA, DEVG's nine installed native executables matched the released
build hashes and its agent service was running. A read-only DEVR-to-DEVG preview
used authenticated UDP throughout a 90-second run:

| Observation | 0.7.11 TCP run | 0.8.1 UDP run |
| --- | ---: | ---: |
| Received frames / 90 s | 285 | 2467 |
| Mean received frames/s | 3.17 | 27.41 |
| First observed picture from tool start | 11.797 s | 3.750 s |
| Largest observed change-to-change gap | 9.031 s | 0.094 s |

These runs used uncontrolled live desktop content at different times, not an
identical-motion A/B benchmark. Presentation changes are polled by a developer
tool; their counts and gaps are not direct physical-scanout instrumentation.
0.8.1 reported zero sequence gaps, queue overflows, age expirations, source
discards or media errors. A separate source-only receiver confirmed a 60 Hz
negotiated cap with no pressure and zero send-credit waiting; actual acquisition
and encoding were producing roughly 25-31 fps on that desktop. This does not
prove sustained 60 Hz motion, local-USB latency, or loss-free behavior forever.
Installed localhost read-only delivery received 1180 frames in 20 s (59 fps).

VI initially fell back to TCP. An authenticated generated UDP measurement to its
secondary address received zero accepted packets, while a local diagnostic
wrapper observed datagrams arriving from its primary address. Targeting that
primary address instead delivered 417/417 packets at 2 Mbps. This isolated the
wildcard source-address selection corrected in 0.8.2; no endpoint validation was
relaxed. Different-address loopback tests validate explicit local binding.

### Final 0.8.2 deployment confirmation

DEVG, DEVR and VI subsequently installed 0.8.2 through the idle-update path.
DEVG's nine native executable hashes matched the release build. With both ends
on 0.8.2, a bounded DEVR preview received 593 frames in 20 seconds over
authenticated UDP. VI's previously failing secondary address delivered 417/417
generated packets at 2.000 Mbps; its real read-only video check also negotiated
UDP and displayed the desktop (93 received frames over 10 seconds of uncontrolled
desktop activity). No endpoint authentication checks were relaxed.

An additional 90-second run with the installed 0.8.2 client and the then-0.8.1
DEVR source received 2354 frames, with a 125 ms largest observed change gap and
47 ms p95. It reported no sequence gaps, queue overflows, age expirations,
source discards, media errors or TCP fallback. Connection establishment took
2250 ms; the first observed picture arrived 1250 ms after the stream request.
These are live observations with the same measurement limitations above.

Final validation passed all 43 native tests, managed checks and WinUI light/dark
checks. The strengthened address-specific datagram, transport and path-probe
tests also passed, and a delayed-feedback HEVC444 hardware loopback delivered
535/535 encoded frames. Sustained full-refresh motion and end-to-end USB-like
input latency still require their own controlled acceptance measurements.
