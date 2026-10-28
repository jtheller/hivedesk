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
