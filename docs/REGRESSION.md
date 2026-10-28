# Regression suite

Run from the repository in Windows PowerShell:

```powershell
.\scripts\verify.ps1 -Hardware -InstalledBroker -Repeat 3
```

The default first step rebuilds native and managed code, runs every CTest check,
the two managed bridge checks and real WinUI checks in both themes. Repeated
tests then exercise isolated Shell/App instances and input/session teardown.
`-Hardware` adds serial GPU, actual DWM and encrypted loopback checks.
`-InstalledBroker` additionally requires the installed app to be idle and tests
its trusted SYSTEM warm-capture/activation path and network warm lease.

Each invocation writes `results.json` and separate stdout/stderr files beneath
`.local/validation/<timestamp>`. It records the source version/commit, whether the
checkout was dirty, arguments implied by the selected suite, per-check exit
codes and elapsed time. Any failed check exits nonzero. Failed checks remain in
the report; reruns create a new directory. `-SkipBuild` is for targeted follow-up
on already-built binaries and does not count as a complete build verification.

| Area | Automated evidence |
| --- | --- |
| Preferences and WinUI | Persistence, fixed size, minimize, icon, theme, update/GPU async/error/cancel paths |
| Hive and windows | Snapshots, old-peer rejection, first-frame cover, hotkey stagger, reorder, drag out, resize/maximize/restore |
| Taskbar | Local desktop without viewer, outside click, Start overlap, repeated open, drag exclusion, fade completion, explicit return and z-order cleanup |
| Input | Direct real hook callbacks on an isolated station; local pass-through, Escape down/up, injected-event exclusion, remote key/button ordering, fixed kick/escape; independent sender and broker queues |
| Clipboard | Isolated native Unicode clipboard, cross-thread completion, empty/text replacement, no listener echo; actual encrypted App stream starts require a preceding clipboard handoff |
| Sessions | Three encrypted generated peers, prewarm promotion, rapid selection/cancel/windowed drop, teardown, reconnect state machine, stale-route rejection |
| Capture/codec | Actual Desktop Duplication, NVIDIA HEVC 4:4:4 encode/decode, RGB/chroma readback, rotation/cancellation of asynchronous slots, automatic CBR quality, generated motion and hard cuts |
| Presentation | Actual DWM: stagger, drag gap, aged warm-frame promotion, fresh Present, cadence enable/disable, taskbar crossfade pixels and unchanged foreground |
| Backpressure | Denied-credit wake/stop, aged-frame discard and fresh IDR, throwing callback teardown, adaptive pressure/recovery, delayed-feedback loopback |
| Discovery | Actual local broadcast discovery and channel replacement after listener/initiator restarts |
| Restart/update | Stuck fixture process termination, settings recovery, signed manifest/package rejection, idle/reconnect guards, managed update results |

The hardware checks may briefly show small generated patches without activating
them. They do not move the physical cursor, inject OS input, change the clipboard,
lock Windows, or change the monitor's refresh rate. Installed warm checks do not
take over a live controller. Do not run the legacy interactive Shell/host/reconnect
probes on the user's working desktop as a substitute for these isolated checks.

Passing this suite is not a proof of every real-world interaction. The isolated
App stress test verifies startup/cancellation and decoder lifecycle; its station
cannot scan out, so Present completion is injected after actual GPU drawing.
It now requires final readiness after 72 rapid Peek transitions and three
same-peer recoveries while old graphics retirement is deliberately delayed.
Actual DWM is tested separately, including twelve preview replacements with
distinct pixel checks, rapid fade reversal/promotion and asynchronous retirement.
Physical Start-menu clicks,
lock/PIN/unlock, two-machine long-duration motion and input-to-display latency,
high-RTT remote hardware, AMD/Intel production streaming and early R500 drivers
still require their corresponding environments. Capability enumeration is not an
AMD/Intel streaming test. Report those gaps explicitly rather than marking them
passed, and never replace a failed result with an unsupported zero-regression claim.
