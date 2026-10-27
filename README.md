# HiveDesk

Windows-native personal LAN desktop switching.

Starting with **0.2.0**, regular releases use `0.x.y`: fixes and refinements
increment the patch (`0.2.1`), and feature releases increment the minor (`0.3.0`).
Alpha/beta/rc labels are reserved for explicitly designated preview builds.
Existing alpha installations can upgrade through the same signed, idle-only OTA.

[Download the latest Windows installer](https://github.com/jtheller/hivedesk/releases/latest)
from the [public repository](https://github.com/jtheller/hivedesk).

0.4.0 warms the visible Hive computers before selection, retains their media
resources after a single decoded picture, and promotes the selected session
without rebuilding its decoder/window. Debug adds bounded per-frame and input
traces. HEVC 4:4:4 codec settings are unchanged. See the
[measurement and warm-session behavior](docs/EXPERIENCE.zh-CN.md) and
[current scope and outstanding work](docs/STATUS.zh-CN.md).

Alpha.36 moves **Relative mouse** into Settings as a saved checkbox, adds
**Restart HiveDesk** to the tray menu, and shows viewer/host versions at the top
of the debug overlay. Different versions are amber; older peers that do not
announce a version show **Unknown**.

Alpha.35 handles NVIDIA access-denied errors when saving the GPU performance
preference. Windows requests permission for the small settings helper only when
needed. Failed saves no longer automatically suggest restarting HiveDesk.

Alpha.34 fixes duplicate cursor rendering when Windows includes the pointer in
the captured desktop. Cursor composition follows each encoded image and is
applied after successful presentation, including dropped frames and display
backpressure. Update both computers; older peers retain their prior behavior.

Alpha.32 adds **Check for updates** beside the Settings version. It verifies the
latest signed release immediately and requests an earlier automatic update check
when a newer version is available. Installation still waits for idle connections.

Alpha.31 separates encoder throughput from frame latency, carries encoded-output
completion timing to distinguish source work from network backlog, and makes
source status reporting nonblocking. Receiver decode, color conversion and
display scheduling use bounded handoffs; display selects the latest completed
image instead of replaying a decoded-image queue. Update both computers for
timing provenance.

Alpha.30 includes the lock-screen fixes and explicitly retains the first-IDR
requirement through capture timeouts after an encoder restart.

Alpha.29 fixes the input desktop access mask and preserves video frame sequence
numbers across lock/unlock capture restarts. Upgrade the controlled computer;
the user confirmed working remote PIN entry and login with alpha.30.

Alpha.28 adds a live ~16ms diagnostic HUD, metric warning colors and a translucent,
click-through background. Update both computers for fast source diagnostics.

Alpha.27 introduced automatic signed updates. Install alpha.27 or newer once;
afterward the service checks the latest GitHub Release every 15 minutes (first
check 60–90 seconds after startup). Downloads happen in the background. Installation
waits for all inbound/outbound remote sessions, queued starts and reconnect intents
to end, closes the idle tray gracefully, installs without UAC/UI or reboot, and
resumes the tray as its original ordinary user. Idle LAN discovery connections do
not block updates. See [release and update operations](docs/RELEASING.md).

Open HiveDesk on each computer. Devices on the same IPv4 LAN discover and
connect automatically; no Network Key, pairing step, server or manual address.
Both computers need **alpha.11 or newer**. The keyless protocol deliberately does
not connect to older shared-key versions.

This is a trusted-LAN tool: any on-link computer running this version can control
the others. Connections use fresh ECDH P-256 session keys and AES-GCM encryption;
there is no user/device identity approval or protection from an active LAN
impersonator. No universal shared password is embedded in the application.

## Current scope

- Windows x64; NVIDIA HEVC 4:4:4 encode/decode capable hardware on both endpoints.
  Alpha.8 targets early R500-series and newer drivers: NVENC/NVDEC API 11.1,
  driver-loaded PTX 6.4, and no CUDA runtime dependency. R500 hardware validation
  remains pending; current-driver RTX 5060 checks pass. The primary display must
  be attached to the NVIDIA adapter.
- Primary display only, SDR, HEVC 4:4:4. No silent 4:2:0 fallback.
- Automatic discovery across active IPv4 interfaces; on-link connections only, encrypted control and video.
  Alpha.25 peers also negotiate a separate encrypted input connection on the same TCP port.
  Alpha.17 retries failed discovery connections with a 3-second cooldown and
  rediscovers restarted peers without restarting the other computer. Broken
  control/video pairs are closed together. Alpha.19 resumes the selected desktop
  after reconnection, with a fresh video session and clipboard handoff. Escape,
  local desktop selection, opening Settings or selecting another computer cancels
  the previous retry. TCP keepalive detects silent transport loss without treating
  a static screen as a disconnect. Failed stream starts retry with 1–8 second backoff.
- Native tray and Win8-inspired top-left switcher: compact 200 DIP rail, 16:9 cached previews,
  machine-name corner badges and live Windows accent color. DirectComposition
  starts on the first corner sample (16 ms timer, no hover dwell) and
  runs the 240 ms fade-in, 240 ms additional-list fade and 200 ms fade-out on
  DWM's clock. Background bounds stay fixed; hotkey cards additionally slide/fade
  in with staggered starts, all finishing by 300 ms. Re-entry
  reverses from the current opacity. Escape and desktop selection dismiss immediately.
- US keyboard scan codes, remote-side IME, Unicode text clipboard handoff.
- Alpha.25 moves physical keyboard/mouse capture and dispatch onto dedicated
  workers, independent of the switcher, clipboard, decoder and renderer. Input
  uses its own AES-GCM TCP connection with TCP_NODELAY and an independent local
  SYSTEM broker pipe. The broker binds that pipe to the media client's process,
  session and increasing event sequence, then injects on the current input desktop.
  At most eight packets await completed-injection ACKs; pending input is bounded
  to 128 network events and 256 capture events. Adjacent absolute motion replaces
  old positions, while relative motion accumulates; neither crosses a key,
  button, wheel or release boundary. A 2 s input stall/expiry closes the input
  session and releases held keys/buttons instead of replaying stale input.
  Clipboard handoff completes before input is enabled on a newly selected
  computer. Later clipboard work stays on the control path and does not block
  the input reader; no cross-channel ordering guarantee is added for live paste.
  Upgrade both endpoints for this path. Older peers retain control-channel input.
- Alpha.12 adds a local SYSTEM desktop broker for capture and input on the
  signed-in console session, including its lock screen. The network agent and
  settings remain ordinary-user processes. Unlocking still requires the Windows
  password/PIN; Ctrl+Alt+Del secure-attention generation is not implemented.
- Opening the switcher requests one fresh primary-screen snapshot per online peer.
  Cached previews appear immediately; replies fade in over 180 ms. Snapshots use
  native-resolution JPEGs (up to 2 MiB, reducing JPEG quality before pixel size),
  with capability-gated 672x378 fallback for older peers. Full-screen waiting
  uses the native image; sidebar fades use separate small bitmaps. Native image
  allocation is bounded to 8192 pixels per edge / 32 Mi pixels, with a 128 MiB
  full-image cache. Snapshots do not start a video/control session.
  There is no periodic preview capture while the sidebar is closed or left open.
- Remote native cursor shape, hotspot and visibility are synchronized separately
  from video. Absolute mouse motion stays local and immediate; host-side physical
  pointer moves synchronize without echoing injected controller moves. Recent local
  movement and sidebar/settings interaction take precedence over host positioning.
  Relative mode keeps the local cursor hidden. Returning locally restores the arrow.
  Both endpoints use the current keyless protocol.
- Settings contain computer name, login startup, tray visibility, the Show Hive shortcut and an optional Debug overlay.
  Alpha.33 adds a diagnostic **Prefer maximum GPU performance** checkbox for this
  computer's HiveDesk host/viewer NVIDIA application profiles. It reads the driver
  state, preserves the previous policy and restores it when unchecked. A changed
  policy queues a video-process restart once remote sessions are idle; Settings
  closes and reopens after restart. Save other form changes before toggling it.
  This preference does not guarantee a fixed GPU clock or P-state.
  The heading shows the installed version, including its alpha revision.
  The window has a fixed size (DPI-scaled), supports minimize and disables resize/maximize.
  They use real WinUI 3 controls, system light/dark theme and accent, with
  Mica on Windows 11 and a themed background on Windows 10. Open from the tray,
  Start menu or by opening HiveDesk.exe, even when the agent is already running. The Win8-inspired switcher remains native.
- Ctrl+Alt+physical backtick key returns directly to the local desktop.
- Ctrl+Shift+Alt+physical backtick disconnects the controller of this computer.
  This fixed shortcut accepts physical keyboard input on the normal desktop,
  releases injected keys/buttons, and leaves your own outbound viewer alone.
  Update both computers to alpha.22 or later for the kicked viewer to return locally and
  stop automatically retrying; selecting the computer again starts a new intent.
  The shortcut does not run on the Windows lock/UAC secure desktop.
- Streaming is automatic: the target is the lower of the two
  primary displays' refresh rates, including fractional rates. Physical display
  modes are only read, never changed. FPS, bitrate and codec tuning remain automatic.
- Alpha.23 uses automatic low-latency CBR for HEVC 4:4:4. Software chooses the
  target from resolution/refresh, then adjusts it using fresh receiver delivery,
  send pressure and observed frame size/QP. No bitrate control is exposed.
  The provisional quality target is QP18 with a maximum-QP guard of 22; this is
  lossy, not mathematically lossless or a guarantee for every image. Resolution
  and chroma stay unchanged. No filler is sent: actual traffic can be below the
  target on easy/static content, and the target is not a hard packet-rate limit.
  Alpha.24 also sets a minimum QP of 16, retaining target QP18 and maximum QP22,
  to avoid spending extra bits at QP2-4 on already easy content. This retains
  precision headroom above the QP18 reference; it is still lossy encoding.
- Encoder and decoder/render capacity reduce capture/encode pacing. With alpha.23
  on both endpoints, sustained send pressure first adjusts the CBR budget; a
  quality-aware FPS ceiling applies when that budget cannot support the target.
  Static-desktop traffic is never treated as available link capacity. Rate changes
  retain the encoder session, drain at most two GPU jobs and do not force an IDR.
  Duplicate receiver snapshots cannot repeatedly cut FPS, decoder samples expire
  after one second, and a recovery request alone does not lower the cap. Older
  feedback or hardware without dynamic bitrate support retains pacing fallback.
  Alpha.16 keeps feedback-transit excess diagnostic-only to avoid false congestion
  reductions. The controlled computer must be upgraded for this pacing fix.
  Alpha.20 debounces encoder-only pressure using fresh samples, expires stale costs
  and probes recovery faster while retaining immediate receiver/backlog protection.
  Source pacing uses a cancellable high-resolution timer; broker/video queue credit
  wakes waiting threads directly. GPU color interop uses its own CUDA stream and
  Desktop Duplication releases each acquired frame just before the next acquisition.
  These changes preserve 4:4:4 and the physical min-refresh target.
  Alpha.21 separates source submission, encoded-output collection and delivery.
  Four NVENC IO pairs support at most two in-flight jobs; completion events are
  used when available, with a separate output worker for synchronous fallback.
  There is no raw captured-frame FIFO. Completed video is bounded by count and
  age; losing a reference picture discards its dependent tail until a fresh IDR.
  Source recovery drains obsolete GPU jobs before submitting that IDR.
  Alpha.24 reserves each output slot before GPU submission: queued pictures plus
  reserved completions total at most two, including the handoff after a GPU slot
  is released. That handoff cannot accidentally overflow the completed queue and
  force reference recovery. Pointer-only Desktop Duplication acquisitions reuse
  the cached desktop instead of copying/encoding unchanged pixels. The initial
  image, software-rendered cursor changes and requested recovery IDRs still work.
  Pacing retains its scheduled phase across normal wake-up jitter and skips
  missed ticks after a stall instead of accumulating delay or catching up.
  Video TCP writes allow two overlapped packets and a 2 s watchdog; a stalled
  partial write closes the connection instead of replaying old bytes later.
  The viewer rejects aged backlog before decoding and presentation, retains only
  the latest prepared picture while Present is busy, and retries that picture
  without waiting for another desktop update. Two seconds of failed freshness
  recovery requests a new video session through the existing reconnect mechanism.
  Alpha.24 allows an already prepared, fresh picture to present when newer
  compressed data arrives during conversion. Undecoded data no longer suppresses
  every completed picture during continuous motion; age and session/IDR-epoch
  rejection remain active, and obsolete pictures can still skip conversion.
  A compact, non-interactive top-left indicator
  appears after the cap stays below 80% of the negotiated target for one second:
  Device busy or Slow connection. It clears after three healthy seconds.
  Idle desktop frame counts do not trigger the indicator.
- Alpha.14 adds a saved **Debug overlay** toggle (off by default). During remote viewing,
  a click-through top-left panel shows target/cap FPS, receive/decode/present FPS,
  stage timings, video bitrate, queue/recovery state and the reason for the cap.
  Source encode/capture/send statistics require alpha.14 on the controlled computer;
  older peers still connect with source detail marked unavailable. Idle desktops
  send fewer frames; Present counts API submissions, not physical display scanout.
- Alpha.17 splits source Encode timing into Color/sync and NVENC/wait, with sample
  age and stale marking. Both endpoints need alpha.17 for these extra fields;
  compatible older peers retain the original diagnostics layout. These are CPU
  wall-time moving averages, not isolated GPU execution measurements.
  In alpha.21 these show per-frame pipeline residence; the pacer separately
  estimates service capacity from overlapping completions and color work, so
  having two jobs in flight does not incorrectly halve the estimated throughput.
- Alpha.23 adds target/actual bitrate, average QP, rate decision, receiver service
  sample age, and separate gap/age/overflow recovery counters to diagnostics.
  The new network fields are capability-negotiated; upgrade both computers for
  the full controller. Debug remains optional and contains no performance knobs.
- Alpha.24 local JSON separates video encryption, overlapped WSASend completion,
  unsent queue residence, submission gaps and send-credit readiness gaps, with
  p95/max values and packet/expiry counts. Viewer JSON adds NVDEC map wait,
  color conversion, draw work and successful-Present interval p95/max. These are
  local wall-time diagnostics, not remote display acknowledgements or scanout.
  Read each endpoint's local log to inspect its own sender/viewer stages; the
  extra details do not require a new network protocol or driver API.
- Alpha.25 adds two input timing lines to the optional Debug overlay. Local JSON
  includes capture-to-send and send-to-ACK roundtrip p50/p95/p99/max, host queue
  and injection p95/max, queue/in-flight counts and sample age. The capture clock
  starts in the hook/Raw Input handler; send timing starts at socket submission.
  ACKs follow completed Windows injection (or an idempotent release), and include
  the return network trip. Injection timing includes input-desktop attachment.
  These are software-stage measurements, not one-way latency, application
  consumption, USB polling latency or input-to-photon latency. The feature adds
  no performance settings and retains alpha.24 video/refresh/quality behavior.
- Alpha.18 writes local performance counters while Debug overlay is enabled:
  `%LOCALAPPDATA%/HiveDesk/diagnostics.json` is the atomic latest snapshot;
  `diagnostics.jsonl` plus `diagnostics.previous.jsonl` retain up to 4 MiB of history.
  Sampling is once per second plus session/state changes, on a dedicated writer.
  No image, clipboard or input-event content is recorded. Run
  `scripts/watch-diagnostics.ps1 -Seconds 60` in a source checkout to follow it.
- No audio, multi-display support, sleep recovery or nesting detection.

## Run

Use Setup.exe on each controlled computer: it installs the local desktop broker
and private-LAN firewall rules. The ZIP alone can view other computers but cannot
host a stream without the matching installed broker. On first run, login startup
and the tray icon are enabled. Startup uses the current user's Windows Run entry;
its settings switch requires no elevation. The broker service starts with Windows
but does not open network ports or start the tray agent. There is no remote access
before a user signs in or after that user signs out. Install alpha.12 on the
controlled computer to enable lock-screen control; alpha.11 viewers remain compatible.

Settings has one text field: **Computer name**. Saving a new name updates connected
peers without restarting their streams. **Start when I sign in** and **Show tray
icon** control background startup and tray visibility. With the tray hidden, open
HiveDesk.exe again to bring up Settings; only one agent runs per user session.
Double-click the tray icon to open Settings. Single-click opens the computer list.

**Ctrl+Alt+H** toggles the computer list. Click the shortcut button in Settings,
press a new combination, then Save. Ctrl, Alt or Win plus a letter, digit or
F1-F11 is supported; occupied shortcuts are rejected without replacing the
previous one. A summoned list stays open while you move to it; Escape, the same
shortcut, a selection, or an outside click dismisses it. The top-left gesture
keeps its normal pointer-leave dismissal.

TCP uses port 47980 for separate control/video/input connections (input requires
alpha.25 on both endpoints); discovery uses UDP
47979 and broadcasts on each active interface. Installer firewall rules are
restricted to the local subnet on Private/Domain network profiles.

Hover at the primary screen's top-left corner for the previous-computer card,
then move down along the left edge to expand the computer list. Click a computer
to enter its desktop. Right-click the sidebar or tray for Settings. For games,
enable Relative mouse in the tray menu; escape back locally before
using the corner in that mode. Ctrl+Alt+physical backtick always returns locally.

The active desktop's copied text updates your local cache. Switching sends that cache to
the next computer when switching. During a lock screen or a temporarily busy
clipboard, the handoff stays cached and retries on the normal desktop without
blocking input. Images/files are ignored. Remote text intentionally replaces the
target clipboard once available.

## Build locally

Install VS Build Tools with MSVC x64, Windows SDK and CMake/Ninja, and the .NET 10
SDK for the WinUI settings helper. The script checks the per-user
TRG/Toolchains/dotnet10 installation first, then dotnet on PATH.
Neither building nor running requires the CUDA Toolkit. The source pins public
NVIDIA codec API 11.1 headers; driver binaries are loaded from Windows, never bundled.
Run scripts/build.ps1 -Package in PowerShell. Inno Setup 7 is required to build Setup.exe.
The clean dist/HiveDesk-<version> folder contains the installer, Chinese installation
guide and its SHA-256 checksum for distribution. The full file ZIP is kept separate.
No hosted CI is required. Use --lan for an explicit real-interface automatic-discovery check. HiveDeskCheck.exe supports --transport, --media, --capture and --render.
Use --colors to verify driver-only GPU color conversion against a CPU reference,
including non-block-aligned images, padded planes and buffer guards.
Use --throughput for a fixed-texture codec benchmark, not a remote latency measurement.
Use --pipeline for native-async and forced-sync NVENC slot ownership, frame order,
changing-color NVDEC verification and encode-only pipeline observations.
The development HiveDeskMediaFreshnessCheck.exe deliberately blocks delivery for
250 ms and verifies that recovery discards aged work and emits a fresh IDR.
Use --media-auto for automatic-CBR and CQP18 baseline hardware verification.
Use --auto-quality for identical offscreen text, chroma and moving-detail frames
through both modes, reporting regional PSNR, QP, bytes and encode residence.
These synthetic measurements do not replace visual review or a real LAN test.
Use --motion for deterministic 2560x1440 window dragging, scrolling and hard cuts,
72 frames per phase after warmup, through NVENC, NVDEC and a hidden renderer.
It checks frame identity and rendered markers and reports stage p50/p95/max,
QP and maximum frame size. Generation/upload and proof-readback frames are
excluded from stage timing; this serial test measures neither LAN performance
nor physical display latency. Complex transitions can still exceed a frame's
time/bit budget even when the configured nominal CBR target is unchanged.
Use --adaptive for
the pacing/indicator controller, and --display to read the primary refresh rate.
The development HiveDeskLoopback.exe --automatic probe tests capture through
render plus injected overload/recovery on one machine, with a temporary test window.
The developer-only --background-automatic, --background-feedback-delay and
--background-cqp-baseline variants use a hidden window and periodic IDRs; their
frame counts are integration checks, not desktop throughput benchmarks.
Use --desktop-aux for JPEG/native cursor checks, and --snapshot for a real one-shot
primary-display snapshot check with a temporary colored test window.
Both packages include the .NET, Windows App SDK and app-local Visual C++ runtimes
for Settings; the receiving computer needs no SDK installation. These runtimes
increase package size. Build verification also launches light/dark WinUI probes
using temporary preview preferences, without modifying your saved preferences.

For an installed desktop-broker check, run HiveDesk.exe --host-check while no
remote stream is active. It consumes its synthetic test input before applications
receive it and writes PASS/FAIL to log.txt; it does not lock the PC or alter the clipboard.

See docs/VALIDATION.md for exact validation and known limitations. Logs and settings
are stored under the current user's LOCALAPPDATA/HiveDesk directory. Logs rotate at 1 MiB.
