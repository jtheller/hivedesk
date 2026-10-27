# Alpha validation

## Alpha 36 preferences, manual restart and endpoint versions

- Relative mouse is a persistent local checkbox in WinUI and native fallback
  Settings. Save/load and startup use the same preference; old configuration
  files default to absolute input. The tray menu now offers Restart HiveDesk.
  Manual restart uses normal shutdown/input release and starts a quiet successor
  that waits for the predecessor to exit; duplicate queued restarts are ignored.
- Version announcements use an optional capability on the encrypted control
  channel. Existing hello/challenge and media formats are unchanged. Debug shows
  separate local-viewer and remote-host versions, amber on mismatch and Unknown
  for peers without version information. Disconnect metadata is copied under
  the same lock used for live peer metadata updates.
- Release build and 30/30 native checks passed. Coverage includes isolated INI
  persistence/legacy defaults, quiet and Settings-opening successor processes,
  predecessor exit ordering/handle isolation, encrypted bidirectional version
  exchange and malformed/unnegotiated/wrong-channel version rejection.
- Hidden debug rendering passed at 100/150/200 percent DPI, including version
  labels, unknown/mismatch tones and bounds. Its generated image was visually
  inspected. Private-desktop light/dark WinUI checks verify checkbox placement,
  save/reload/disable and a visible Save button in the fixed-size window. Managed
  update/GPU bridge checks also passed. No live desktop input or tray restart
  was injected; physical two-machine interaction remains user acceptance.

## Alpha 35 GPU policy save permissions

- NVIDIA access denied (-175) is mapped explicitly even when driver error text
  is blank. A denied ordinary-user mutation retries once through Windows UAC;
  status reads, other errors and already elevated denials never prompt again.
  The failed transaction releases its mutex before the elevated child starts.
- The child accepts only enable/restore for sibling HiveDesk executables and
  requires the same Windows account. Backup filesystem operations impersonate
  that account's filtered token; backup content cannot expand application scope.
  Successful child exit is followed by an independent ordinary-user readback.
- UI failure recovery offers restart only for a verified state transition or
  pending restart. Cancellation and unchanged denied saves preserve the actual
  checkbox without suggesting that a restart will fix the permission failure.
- Release compilation and 29/29 native checks passed. Permission regressions
  exercise exact retry/action routing, cancellation, no recursive escalation,
  same-account rejection, exception-time mutex release and backup scope checks.
  Managed bridge checks and private-desktop light/dark WinUI checks passed,
  including cancelled approval and unchanged-save feedback.
- No UAC approval or real driver mutation was performed during this validation.
  The reported -175 machine still needs a real approve/save/restore comparison;
  administrator approval cannot guarantee a damaged/restricted driver database
  will accept the write. Existing local policy was inspected without changing it.

## Alpha 34 cursor composition

- DXGI pointer ownership is read only when LastMouseUpdateTime is nonzero.
  Ownership changes force a fresh desktop copy, even without a desktop-update
  timestamp; cached recovery IDRs retain the ownership of their copied image.
- Each NVENC job, local broker frame and negotiated video packet carries cursor
  composition. Older peers retain their exact previous wire layouts. Local
  broker framing rejects mixed binary versions instead of misreading metadata.
- Only a successful Present publishes ownership to the native cursor layer.
  Prepared-frame replacement, cancellation, busy/occluded retries and early
  startup cannot apply a discarded picture's state. Session/generation/sequence
  guards reject stale UI notifications. Cursor shape/motion keep their separate
  path; native shape replacement respects embedded and relative-mode hiding.
- Release compilation and 28/28 CTest checks passed, including malformed wire
  data, feature negotiation, encrypted transport, broker round trips and the
  new capture/presentation state regressions. Real hidden-window NVENC/NVDEC
  tests passed with cursor metadata rotating through native asynchronous and
  forced-synchronous encoder slots. Real GPU rendering plus injected Present
  results verified success-only ownership and color/resize correctness.
- Managed update/GPU bridge checks and private-desktop WinUI light/dark tests
  passed. No live desktop input or visible capture probe was used. Successful
  Present is not physical scanout proof: a compositor/UI tick can still separate
  native cursor and video transitions. The reported VI/DEVG drag ghost has not
  been reproduced in this validation; two-computer acceptance remains pending.

## Alpha 33 GPU power-policy comparison

- Added an immediate WinUI checkbox for local HiveDesk host/viewer NVIDIA
  application profiles. The original explicit or inherited policy is journaled
  before writes; external conflicts are refused. No global power settings,
  display modes or encode/decode algorithms are changed.
- Native CTest passed 27/27 checks. Transaction tests cover inherited/explicit
  restoration, interrupted writes, post-commit journal errors, repeated actions,
  unrelated settings and external profile/value changes. Restart tests use
  isolated subprocesses to verify predecessor exit ordering, restricted handle
  inheritance, idle admission, Settings races, event owner/ACL validation and
  persistent pending requests.
- A real NVIDIA DRS integration probe passed on the local RTX 5060 driver using
  unique nonexistent temporary executable paths. It verified maximum-performance
  readback, exact restoration of explicit/inherited preferences and removal of
  all temporary profiles. Actual HiveDesk application profiles were unchanged.
- Managed GPU/update bridge tests and real WinUI light/dark checks passed. The
  UI ran on a private non-input desktop; tests cover checkbox placement, duplicate
  submissions, actual-state reconciliation, unsaved preferences, restart retry
  and recovery finalization. No desktop input was injected.
- VI/DEVG clock behavior and stutter improvement remain to be compared on the two
  physical machines. A preference is not a guarantee of fixed clocks/P-state;
  early R500 driver compatibility was not re-tested on hardware in this round.

## Alpha 32 manual update check

- Settings adds one action beside the version without changing its fixed size.
  The native helper verifies the signed public release; newer versions signal a
  separate SYSTEM-owned event that wakes the existing update worker. The current
  idle-session admission, download verification and installation path are reused.
- Release native compilation and 25/25 CTest checks passed. The update test adds
  invalid/untrusted wake-handle rejection without changing those events' state.
  The actual helper successfully verified the public release and returned the
  structured current-version result.
- The managed bridge checks passed LF/CRLF status parsing, malformed/version
  rejection, async completion, failed helper exit, timeout, caller cancellation,
  a stuck task and missing helper. No test starts the installed SYSTEM updater.
- Actual WinUI light/dark self-tests run on a private non-input desktop and check
  the version/button bounds, disabled busy state, duplicate-request prevention,
  current/requested/unavailable/error messages and existing preference controls.
  The launcher never switches desktops or sends keyboard/mouse input.
- No new live privileged wake/download/install cycle is claimed by these tests.
  Alpha.31 rendering behavior is unchanged; physical rendering cadence remains
  a separate two-computer measurement.

## Alpha 31 pipeline scheduling and freshness

- Encoder capacity is observed only when GPU slots block the producer beyond
  its scheduled capture deadline. Completed-frame latency remains diagnostic.
  Deterministic regressions cover sparse-to-continuous 100 Hz work with 15 ms
  residence, genuine saturation, network blocking, stale evidence and recovery.
- Negotiated completion timestamps preserve the original capture timestamp and
  exclude intrinsic source work from transport-backlog age. Tests cover both
  mixed-version directions, the encrypted transport path, malformed timestamps,
  stale TCP tails, source expiry and output-only refresh without losing valid
  P-frame references.
- Receiver submission, mapping/conversion and presentation have bounded
  ownership. Two decoder jobs and three RGB surfaces are the upper bounds;
  there is one replaceable ready picture. Display waits precede backbuffer
  drawing, and a hidden first frame can announce readiness without waiting for
  the shell to reveal the window. Physical scanout is not measured by Present.
- Receiver rate feedback reports actual compressed admission backlog, including
  a frame already removed from the queue while waiting for decoder capacity.
  A 250 ms evidence latch survives queue flush/dequeue without being refreshed
  by feedback polling. Decoder map/conversion p90 remains diagnostic only.
- Status reports use a latest-only mailbox. Named-pipe tests deliberately block
  status/video writes and check that publication stays bounded and shutdown
  cancels I/O before joining.
- Release native build and 25/25 CTest checks passed on 2026-10-26. The GPU async
  check verifies parser/map overlap, two-job admission/cancellation, ordered
  colors, format-change drain, three-texture rotation and resize, static retry,
  error propagation and hidden live-view readiness/close. A separate concurrent
  ownership check covers 50,000 publications and atomic presenter replacement.
  Freshness regressions also prevent an older output from re-arming recovery
  after a newer picture has become ready.
- Additional actual GPU checks passed for native-async and forced-sync NVENC,
  immutable source-completion timestamps, HEVC444 render readback and resizing.
  The background host probes passed 12 credit-gated frames and 250 ms blocked
  delivery recovery: obsolete outputs were discarded and the resumed IDR had
  4.2 ms capture age. These are local integration observations, not LAN latency
  or general-content frame-rate measurements. No visible stimuli or desktop
  input were generated, and the installed app/service was not manually restarted.
- The user confirmed alpha.30 remote lock-screen PIN entry/login and OTA work.
  This release's two-PC cadence, long-running behavior and physical display
  timing still require use on VI and DEVG; synthetic/headless checks cannot
  establish those results. No physical display mode is changed.

## Alpha 27 public release delivery and idle OTA

- Public repository and releases use `jtheller/hivedesk`. Local packaging now
  publishes by default, pins the source commit, signs the manifest and finishes
  all draft asset uploads before exposing the release as latest. No CI job is used.
- The SYSTEM service launches the isolated native updater. Fixed HTTPS release
  URLs, a compiled RSA-3072 key, strict version/filename parsing, signed SHA256 and
  exact size checks protect installer selection; no user-supplied install path
  or remote command is accepted by the update workflow.
- Idle admission covers inbound/outbound, queued selection and reconnect intent.
  App-side locking prevents a new Start from racing the final idle decision;
  a SYSTEM-owned startup barrier excludes new agents during installation. The
  helper observes process exit rather than terminating active agents and restores
  ordinary-user trays after setup, with no automatic reboot.
- Native build and 20/20 CTest checks passed. The new check covers signed fixture
  verification, altered/truncated signatures and packages, numeric prerelease
  ordering, malformed/path-traversal manifests, activity deferral policy and
  rejection of user-owned coordination events.
- This validation does not install or restart the live app/service. Actual SYSTEM
  setup, user-token restart and remote-session deferral remain post-bootstrap
  acceptance work; there is no claimed transactional rollback guarantee.

## Alpha 26 overlay placement

- Debug, stream-pressure and reconnect overlays share one native window and now
  anchor 16 DIP from the primary screen's top-left corner, scaled with system DPI.
  The positioning guard also checks the origin, not only the current size.
- This release changes the overlay position and version metadata. The existing
  interactive debug probe's height expectation is corrected from 350 to 390 DIP
  to match Alpha.25's two extra input-timing lines.

## Alpha 25 independent input path and completion measurements

- Physical low-level keyboard/mouse hooks and relative Raw Input run on a
  dedicated capture message thread. A separate sender drains a bounded queue;
  UI composition, snapshot work and clipboard operations are not performed in
  hooks. Physical escape/kick and Show Hive actions post to the UI owner.
  Every dispatched event retains its original peer/session identity, and the
  application verifies that identity again before forwarding it.
- FeatureDedicatedInput negotiates a third AES-GCM TCP connection on the
  existing listener port, with TCP_NODELAY and independent read/write workers.
  An InputEnable barrier on the control connection follows the initial clipboard
  handoff and stream start. Live clipboard work stays on the control reader;
  input dispatch does not take its host/media mutex. This separates concurrent
  work without promising a new ordering barrier for a later live copy/paste.
  Peers without the capability retain legacy control-channel input.
- The capture queue holds at most 256 events, and the network input queue holds
  at most 128 pending events. Up to eight packets may be submitted without a
  completed-injection ACK. Only adjacent motion of the same kind can merge:
  absolute position replaces the prior sample, relative displacement accumulates,
  and key/button/wheel/release boundaries retain order. Relative accumulation
  retains its oldest capture timestamp. Increasing sequences may have gaps
  after merging; stale sessions and repeated sequences are rejected.
- Capture/network queues and outstanding completions have a 100 ms freshness
  budget. Overflow or expiry ends the failed input path and releases held keys
  and buttons instead of silently losing a release or replaying old actions.
  SYSTEM IPC checks receiver-local queue age and checks it again after desktop
  attachment, before SendInput. Session revocation and pipe closure release
  held state. A failed SendInput release remains tracked for a later retry.
- The SYSTEM broker uses a separate small named pipe and a windowless, hook-free
  injection thread. Its handshake binds to the already verified media client's
  process ID and active session; every event rechecks session and sequence.
  Capture startup/restart, feedback and large encoded IPC writes do not hold
  the input pipe's locks. Media IPC failure also revokes its input authority.
- Input ACKs are emitted only after the injection callback completes. Success
  means SendInput accepted the event, or a duplicate release needed no new OS
  event. Rejected/expired input is released before its failure ACK; the broker
  then drains without applying further packets until the client closes. This
  preserves the failure ACK instead of discarding unread pipe data during an
  immediate DisconnectNamedPipe. Production uses SendInput; automated input
  tests supply a recorder and do not inject events into the user's desktop.
- Viewer JSON reports capture-to-send and send-to-ACK p50/p95/p99/max over up to
  256 recent samples within two seconds, plus host queue/injection p95/max and
  bounded queue/in-flight counts. Capture timestamps start in hook/Raw Input
  handling; the send timestamp is immediately before WSASend submission, after
  encryption. Host queue time starts at that computer's input-network callback;
  injection includes input-desktop attachment and SendInput. Send-to-ACK includes
  both network directions, host processing and ACK dispatch. No unsynchronized
  cross-machine timestamp subtraction or RTT/2 one-way estimate is used.
- The optional Debug overlay adds two input lines; Settings changes only its
  version to 0.1.0-alpha.25. Alpha.24's video encoder/decoder, quality bounds,
  freshness limits and minimum physical-refresh policy remain unchanged.
  Primary-screen/US-keyboard/remote-IME/text-clipboard scope is preserved; this
  release does not add audio, multiple displays, virtual HID or display changes.
- New headless checks cover capture dispatch while the owner thread does not
  pump messages, immutable same-peer/new-session routing, adjacent motion merge,
  preserved edge order, expiry/overflow release and bounded shutdown. A real
  named-pipe test blocks a large video write while input completes independently,
  holds a fake injection callback to prove no premature ACK, checks process and
  session binding, sequence gaps/duplicates, false-ACK delivery, expiry and
  queued-tail rejection, release retry, pipe closure and concurrent send/stop.
  Encrypted transport tests exercise the independent lane with a fake host sink.
- Release native compilation and all 19 CTest checks passed on 2026-10-26.
  This includes the real named-pipe/fake-injection test after the failure-ACK
  delivery regression was fixed. The checks install no capture hooks and inject
  no desktop input. Evidence is recorded in .local/alpha25-*.txt.
- Final LAN checks passed three-channel discovery, independent restarts in both
  directions and recovery after the video connection closed. Hidden 2560x1440
  D3D11/NVENC 4:4:4/AES-GCM TCP/NVDEC integration passed automatic overload/recovery
  (366/366 encoded/received frames, zero backpressure, 15 ->59.94 FPS cap) and
  35 ms delayed feedback (541/541 frames, zero backpressure, final 59.94 FPS cap).
  These are integration checks, not a throughput benchmark or physical input
  measurement. The final native build introduced no new warnings, and the
  PowerShell diagnostics watcher passed syntax parsing.
- Installed SYSTEM injection, lock/UAC desktop behavior, physical keyboard/mouse
  latency and test-host-a/test-host-b bidirectional handfeel remain acceptance work after both
  computers are upgraded. Tests do not establish USB hardware equivalence,
  application-consumption latency or input-to-photon latency. No installed app
  or service was stopped, restarted or replaced during development validation.
- API semantics: [SendInput returns input-stream insertion counts](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput);
  [SetThreadDesktop requires a thread without windows or hooks](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setthreaddesktop).

## Alpha 24 motion continuity and stage diagnostics

- Retains HEVC 4:4:4, original resolution, automatic CBR, one-frame nominal VBV,
  P1 ultra-low-latency, and the lower physical primary-display refresh target.
  Physical display modes, NVENC/NVDEC API 11.1, PTX 6.4 and the R500-driver
  compatibility target are unchanged; old-driver hardware was not rerun here.
  Minimum QP is now 16, target/initial QP18 and maximum QP22. The lower bound
  avoids QP2-4 bitrate spending on easy images while allowing more precision
  than the QP18 reference. This remains lossy, not an artifact-free guarantee.
- Desktop Duplication ignores pointer-only acquisitions after copying the first
  image. Hardware cursor metadata uses its existing independent channel; real
  bitmap changes, including software cursors, continue to copy. Cached pixels
  remain available for a forced recovery IDR. The deterministic capture check
  interleaves 1 kHz pointer metadata with 100 Hz image updates and covers initial
  acquisition, unsuccessful first-copy state and cached recovery eligibility.
- Frame scheduling advances the previous deadline instead of anchoring each
  deadline to the actual wake-up. Normal dispatch jitter no longer accumulates
  into a slower nominal cadence; missed ticks after stalls/idle are skipped in
  one step. Tests cover 1,000 jittered frames, long stalls, idle resume, exact
  tick boundaries, cadence changes and the unpaced diagnostic mode.
- Source output admission reserves space before submitting GPU work. Queued
  pictures plus reservations are bounded to two, including a completion between
  releasing its encoder slot and publishing to the output queue. This removes
  a handoff window that could admit extra work and unnecessarily reset the P
  reference chain. Exceptional ownership paths release reservations exactly
  once. Existing capture-age expiry and IDR recovery limits are retained.
- Viewer conversion can still skip an obsolete decoded image, but a prepared
  image remains eligible while fresh even if a newer compressed packet arrives.
  Continuous arrivals during conversion no longer suppress every Present. The
  original age/session/IDR-epoch guards still reject obsolete output. Queue and
  hidden-render checks cover this progress rule alongside busy-Present retry,
  fresh replacement, expiry and replacement-IDR rejection.
- Local JSON adds video encryption, WSASend completion, queue residence,
  submission intervals and readiness intervals (p95/max), packet counts and
  queue expiry. Viewer JSON splits NVDEC map wait, conversion and draw work and
  records successful-Present interval p95/max. These measurements describe local
  stages, not remote display acknowledgement or physical scanout. Sender details
  are read on the sending computer; no new wire extension or driver API is used.
- Release CTest ran all 16 checks successfully. Native-async/forced-sync GPU
  pipeline, hidden renderer, generated quality and motion checks passed. The
  pipeline retains four allocated IO slots, at most two accepted jobs, ordered
  outputs, runtime rate changes without implicit IDRs and clean draining.
- Final QP16-floor comparison uses the same 60 generated 1280x720 frames. CQP18
  reference: 869,123 bytes, overall PSNR 42.730 dB, text/motion 42.385 dB.
  Automatic CBR: 877,143 bytes, overall 41.813 dB, text/motion 41.579 dB.
  Alpha.23's earlier same-fixture CBR observation was 1,420,645 bytes and overall
  43.119 dB; fewer bytes also reduce precision, so this is not a lossless claim.
  The compared fixture does not establish perceptual equivalence for all text
  or real workloads. Both modes check the 4:4:4 stream and only the initial IDR.
- New developer --motion generates 2560x1440 window dragging, scrolling and
  detailed hard cuts: 72 frames per phase after eight warmup frames. It verifies
  every encoded sequence/capture timestamp, decoded counts, only the initial IDR,
  and each phase's final rendered marker. Reports include bytes, maximum frame
  size, QP and color/NVENC/decode/render/Present p50/p95/max. CPU generation,
  completed GPU upload, warmup and proof-readback frames are excluded from stage
  samples. It is serial and hidden: no capture, network, controller adaptation,
  asynchronous-throughput or scanout claim.
- Final QP16-floor motion results: dragging NVENC/decode p95 3.073/1.579 ms;
  scrolling 2.773/1.581 ms; hard cuts 9.953/10.210 ms, with a largest encoded
  frame of 2,730,674 bytes. This demonstrates why large content changes can
  still exceed a 100 Hz frame budget. The nominal CBR target is not a maximum
  encoded packet size, and these changes do not prove all stalls are removed.
- Final encrypted hidden-render loopbacks passed automatic overload/recovery
  (59.94 ->15 ->59.94 FPS cap, 342/342 encoded/received frames) and 35 ms delayed
  feedback (59.94 FPS final cap, 534/534). These are single-machine integration
  checks with periodic IDRs, not a real-LAN throughput or scanout measurement.
  The 250 ms blocked-delivery check skipped two obsolete sequences and recovered
  on a 3.9 ms-old IDR; denied-credit stop took 53.9 ms. Non-standard callback
  failure still stopped and joined cleanly. The PowerShell diagnostic watcher
  also passed syntax parsing.
- Evidence is in .local/alpha24-*.txt, including loopback, feedback-delay and
  freshness logs. Real test-host-a/test-host-b upgrades and both-direction motion validation
  remain pending. The running installed version has not been stopped, restarted
  or replaced as part of development and hidden tests.

## Alpha 23 automatic CBR and fresh receiver feedback

- Automatic sessions use HEVC 4:4:4 CBR, P1 ultra-low-latency, one nominal-frame
  VBV, no filler, no lookahead/B frames/AQ. Source resolution, physical monitor
  modes and the negotiated min-refresh target are unchanged. A resolution/rate
  estimate initializes the budget; QP18 target and maximum QP22 are provisional
  visual-quality guardrails, not a universal artifact-free or lossless guarantee.
- Once per second, the controller uses cumulative P-frame bytes/QPs, advancing
  receiver delivery and actual producer credit wait. Two active pressured windows
  learn 90% of observed goodput, with each bitrate cut bounded to 25%. Decoder
  queues/recovery and static-desktop low traffic do not establish link capacity.
  A separate frame-rate ceiling estimates how many frames fit the quality budget;
  healthy delivery probes upward. Hardware capacity remains a separate pacer.
  Credit waiting is measured separately from waiting for GPU slots/recovery.
- NVENC bitrate/nominal cadence changes retain the session and references, wait
  for at most the two admitted jobs to drain, and set neither forceIDR nor reset.
  Unsupported dynamic-bitrate capability retains initial CBR with pacing fallback.
  Codec API 11.1 and the existing driver-only CUDA/PTX path remain unchanged;
  R500-driver interoperability was not rerun on this development host.
- Receiver snapshot sequences reject duplicates; fresh reports can trigger a
  capacity/backlog decrease once, not on every producer iteration. Decoder
  service samples expire after one second. Recovery requests alone do not reduce
  FPS, while fresh measured overload during recovery still does. Optional wire
  extensions are gated by FeatureAutoRate; old feedback remains accepted with
  pacing fallback. Upgrade both endpoints for the complete network controller.
- Debug/JSON include target/actual bitrate, average QP, decision reason, receiver
  ages, source discarded pictures, and separate gap/age/overflow counters. No
  user performance control is added. Settings only changes version. The debug
  preview fixture includes the new fields and dimensions; its interactive visual
  probe was not launched during this user's active session.
- Release build and all 15 CTest checks passed on 2026-10-26 after review fixes.
  New cases include duplicate/expired receiver reports, independent decode
  overload, idle traffic, recovery flags, constrained delivery/upward probes,
  recovery below 3 FPS and the 92.16-to-69.12 Mbps bounded-cut overflow regression.
  Existing broker/transport checks cover old layouts and truncated extensions.
- Native-async and forced-sync hardware pipeline tests passed: four allocated
  slots, maximum two jobs, 24 decoded changing-color pictures per mode, QP <=22,
  six idle bitrate/fractional-cadence changes without implicit IDRs, and rejection
  of in-flight reconfiguration. Tests also retain ownership/EOS/cancellation checks.
- Identical 60-frame 1280x720 offscreen content (14px text, moving detail, colored
  edges, one-pixel chroma) passed codec comparison. CQP18: 869,123 bytes, overall
  PSNR 42.730 dB, text/motion 42.385, chroma 43.968. Automatic CBR at an initial
  13.824 Mbps target: 1,420,645 bytes, overall 43.119 dB, text/motion 42.957, chroma
  43.642, mean/max frame QP 11.067/22. Both returned all 60 frames with only the
  initial IDR. Regional guards are >=28 dB and at most 5 dB below the baseline;
  these results cover this generated scene, not arbitrary text or real workloads.
- Encrypted hidden-render loopbacks passed automatic negotiation, fresh feedback,
  injected decoder overload/recovery (59.94 ->15 ->59.94 FPS cap), stale-session
  rejection, CQP18 diagnostic baseline, and 35 ms delayed-feedback handling.
  Periodic IDRs stimulate an otherwise idle desktop; observed frame counts are
  integration checks, not a throughput benchmark or a same-scene comparison.
  A final 250 ms blocked-delivery test discarded three obsolete sequences and
  resumed on a 3.9 ms-old IDR; denied-credit stop took 56.3 ms. Callback failure
  still stopped/joined cleanly. Evidence is in .local/alpha23-*.txt.
- The installed alpha.22 process was left running. Two-PC test-host-a/test-host-b motion,
  congestion adaptation, visual quality and long-session oscillation validation
  remain pending. Video TCP send concurrency and queue/age bounds are unchanged;
  no claim is made that this removes all physical-link or driver scheduling stalls.
  Reference: [NVIDIA low-latency rate control and reconfiguration](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/nvenc-video-encoder-api-prog-guide/index.html).

## Alpha 22 fixed local controller kick

- Ctrl+Shift+Alt+physical backtick revokes the current inbound controller on the
  normal Windows desktop. The hook accepts physical events only, consumes repeat
  and key-up, and leaves Ctrl+Alt+backtick as a separate local escape. It does not
  exit an independent outbound viewer. Lock/UAC secure desktop hooks are not added.
- Revocation immediately closes input/video admission. A dedicated worker sends
  the session-scoped termination notice before potentially slow broker teardown,
  releases broker-injected input, joins capture, and clears remaining queued video.
  A revoked lease cannot be replaced by an automatic retry during cleanup.
- FeatureHostKick negotiates the new control message. A matching viewer cancels
  its retry generation and startup wait immediately; delayed/duplicate notices
  cannot cancel a later selection. Selecting the peer again remains possible.
  Old peers lacking the capability are blocked on their current connection after
  a kick; upgrading both endpoints is required for graceful return and retry stop.
- Release build and all 14 CTest checks passed on 2026-10-26, including new fixed
  hotkey/session-control tests and extended encrypted transport loopback checks.
  Coverage includes left/right modifiers, injected-event isolation, autorepeat,
  modifier release, stale peer/session rejection, retry cancellation, reciprocal
  session independence, malformed messages, and retained connection tokens.
- These are state-machine and transport checks, not a physical keyboard / two-PC
  acceptance test. The installed alpha.21 process was left running. Settings UI
  only changes its version; no visual layout changes or repeat visual tests.
- Performance controller/sender repair remains proposed separately. This release
  retains alpha.21 encoding, freshness thresholds, pacing, and send concurrency.

## Alpha 21 bounded NVENC pipeline and freshness recovery

- Source now has separate capture/conversion/submission, NVENC completion, and
  delivery workers. Four input/output pairs are allocated; at most two jobs may
  be reserved/submitted. Windows async completion events are capability-selected;
  unsupported async mode uses the same output worker with blocking bitstream
  retrieval. Pixel format, QP18, no B frames/lookahead, API 11.1 and driver floor
  are retained. The CUDA producer converts into a different YUV slot while the
  preceding job is encoded. Input slots remain owned through bitstream copy,
  unlock and unmap. Accepted GPU work drains before resource destruction.
- There is no raw capture FIFO: the producer acquires the current desktop when
  resources are available. Completed output holds at most two pictures. The
  normal capture-age budget is 100 ms; intrinsically slower encoding is allowed
  its measured processing residence plus only 20 ms delivery slack, preventing
  an endless drop/IDR loop on a temporarily slow GPU. Expiry/overflow abandons
  dependent references and requests a fresh IDR. Fresh IDRs replace predecessors;
  recovery drains the encoder to idle before submitting another recovery frame.
- Per-frame residence remains visible in source diagnostics. Capacity estimation
  uses completion spacing only when the encoder was continuously supplied, with
  color work as another lower bound. Pacing/network idle is not encoder service
  time, and overlapping two frames does not halve the capacity estimate. Recovery
  samples can include IDRs while the source awaits a fresh reference chain.
- Video uses one overlapped scatter/gather WSASend with SO_SNDBUF=0. A queued
  unsent picture ages out after 50 ms; its reference tail is discarded. Encryption
  plus write has a 150 ms budget; failure aborts the video/control pair. Canceled
  I/O completion is awaited before releasing packet/OVERLAPPED storage. A close
  notification race and handshake channel race were also fixed. Socket completion
  is not proof of remote display; zero send buffering can affect TCP throughput
  and needs acceptance on the actual two-machine LAN.
- Receiver bounds pending compressed pictures to four and checks 50 ms local/
  excess transit age at enqueue, dequeue, decode completion and pre-Present.
  Transit uses elapsed differences between unrelated clocks and a session floor;
  it never compares raw clock origins. Epochs invalidate older in-flight decoded
  output after an IDR/reset. Two seconds without successful freshness recovery
  reports a media failure, causing the existing selected-stream logic to start
  a new session. The old baseline is not silently relaxed to accept stale IDRs.
- Present uses DO_NOT_WAIT. If busy, only the latest prepared backbuffer is held
  and retried without another decode or retained NVDEC surface. New frames or
  expiry cancel the old retry. Hidden-window startup still uses fresh-picture
  readiness; presentation counts only successful Present calls, not scanout.
- Twelve CTest checks pass, including synthetic late/overflow/reference-chain
  traces, slow replay, independent clocks/drift, recovery timeout/session barriers,
  real encrypted stalled-reader cancellation, and existing codec/color/reconnect
  tests. Native-async and forced-sync hardware checks verify 24 changing-color
  frames each, two-job admission, slot rotation, timestamps, midstream IDR, EOS
  with pending work, wrapped-slot cancellation, and idle shutdown.
- The explicit background MediaHost test blocks delivery for 250 ms. One observed
  run skipped six obsolete sequences and resumed with a fresh IDR aged 5.5 ms;
  denied-credit shutdown took 57.2 ms. Non-standard callback exceptions stop and
  join cleanly. Static-desktop stimulation uses early forced IDRs; P-reference
  invalidation is separately exercised by deterministic queue tests.
- Hidden render tests verify GPU colors/geometry/resize and injected busy-Present
  retry, including a final static picture, stale cancellation and replacement.
  Automatic and delayed-feedback encrypted loopbacks retain the negotiated
  physical min-refresh target and verify overload/recovery on this machine.
- An illustrative same-build 2560x1440 fixed-texture CQP18 encode-only run measured
  338.6 FPS serialized, 429.6 native pipeline, 429.1 forced-sync pipeline. These
  exclude capture/network/decoder/display; they are not remote FPS or latency.
  The hardware probe's canceled-session fixture initially tried to register the
  same D3D texture in two simultaneously alive encoder contexts; it was corrected
  to destroy/unregister the first session before creating the second.
- This session leaves the installed alpha.20 running. Settings changes only its
  packaged version. Local checks do not replace alpha.21 installation and two-PC
  tests in both directions, especially TCP pacing under real LAN conditions.
  Freshness budgets are not absolute capture-to-photon guarantees: driver calls
  already in flight are not cancellable, and recovery can temporarily pause video.
  References: [NVIDIA NVENC threading and operation modes](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/index.html#threading-model),
  [Microsoft TCP streaming overlapped I/O](https://learn.microsoft.com/en-us/windows/win32/winsock/tcp-ip-specific-issues-2),
  [WSASend completion](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsasend),
  [socket cancellation lifetime](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-closesocket).

## Alpha 20 frame pacing, credit notifications and GPU synchronization

- Replaced the source's condition-variable deadline wait with a cancellable Windows
  high-resolution waitable timer. Stop/keyframe notifications remain latched across
  wait enrollment. The worker rechecks the clock and predicates; no spin loop or
  process/global timer-resolution change. An ordinary timer is the compatibility
  fallback. On this host, 48 requested 10 ms waits measured CV p50/p95
  15.992/16.098 ms versus timer 10.053/11.014 ms. These are wait durations, not
  whole-stream FPS or end-to-end latency.
- BrokerReady wakes the capture worker, and video queue credit wakes the broker
  reader directly, replacing two 2 ms polling loops. Notifications close the
  predicate-to-wait race; bounded fallback waits still sample sustained congestion.
  Queue bounds and video framing are unchanged. Named-pipe tests cover withheld
  credit, spurious notifications, cancellation and publication during wait enrollment;
  the encrypted transport test checks readiness callbacks outside its locks.
- Encoder-only cap reductions require three fresh samples spanning at least 500 ms,
  then decrease at most 20% every 500 ms toward measured capacity. Cached estimates
  expire after one second and do not count as new evidence. Fresh work after idle
  resets the EMA. Recovery probes every 500 ms after one healthy second (two after
  receiver/network pressure), adding at least 5 FPS or 20%, bounded by measured
  capacity and min-refresh. Decoder/backlog/send blocking still react immediately.
- Deterministic tests use the observed 52.863 ms to 4.7 ms encode-cost envelope:
  a short spike does not collapse the cap, persistent overload still converges,
  and recovery returns to the 99.946 Hz target within six seconds. This is a
  controller regression, not a replay of the complete two-PC session.
- Texture map/copy/color/unmap work uses a private nonblocking CUDA stream and
  synchronizes that stream before returning. Global context synchronization is
  removed from this path; resource lifetimes remain synchronous. Desktop Duplication
  retains the acquired frame through encoding/pacing, releasing immediately before
  the next AcquireNextFrame. No CUDA runtime, API-level or driver-floor increase.
  References: [CUDA stream synchronization](https://docs.nvidia.com/cuda/archive/11.4.4/cuda-driver-api/stream-sync-behavior.html),
  [DXGI frame release](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgioutputduplication-releaseframe),
  [Windows high-resolution timers](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw).
- Release build and ten CTest checks pass, including real NVENC/NVDEC HEVC444 and
  GPU color/reference tests. Explicit MediaHost credit tests pass with 12 gated
  frames and stop cancellation. The hidden D3D11 renderer verifies colors, geometry
  and resize. Background automatic and delayed-feedback loopbacks verify actual
  capture/encode/decode, overload recovery and stale-session rejection without
  visible test windows or input. Periodic IDRs provide frames on an idle desktop;
  these probes are not throughput benchmarks.
- Three alternating fixed-texture throughput runs each: alpha.19 160.4/173.7/165.8
  FPS; new CUDA path 169.4/173.0/165.4 FPS. Variation overlaps; no significant GPU
  throughput improvement is established. This excludes acquisition/network/scanout.
- Settings code is unchanged except the version. Self-contained WinUI publication
  is built, but its interactive theme tests are not repeated in this session.
  Running alpha.19 is left uninterrupted. Alpha.20 installation, real two-machine
  rediscovery/reconnection and both-direction slowdown acceptance remain pending;
  test-host-b's NVENC timing spikes are not claimed fixed by these local tests.

## Alpha 19 session reconnection and visible version

- Settings display the packaged assembly version; native fallback and startup log
  use the CMake application version. WinUI self-tests check the rendered version.
- Selected-desktop intent survives transport loss. A new authenticated dual-channel
  connection triggers a fresh decoder, session ID, clipboard handoff and Start.
  Failed starts back off from 1 to 8 seconds; detected decoder errors also retry.
  Escape/local selection/Settings/new selection invalidate pending work immediately.
- A compact Reconnecting badge remains visible during retries. Both UI and command
  generations reject late completion after cancellation. Host disconnect cleanup
  stops capture and releases injected input; connection tokens protect newer sessions.
- TCP keepalive starts after 5 idle seconds with 1-second probes, using the OS retry
  count. This is transport liveness, not a video inactivity timeout or a promise of
  instant failure detection. Physical cable loss and sleep/resume still need a
  two-machine acceptance test; TCP does not detect a live but hung capture process.
- Nine CTest checks and both WinUI themes pass. The explicit ReconnectProbe uses
  real NVENC/NVDEC, the running App, native selection and encrypted loopback: first
  decoded image, peer restart, automatic decoded-image return, unique session and
  clipboard-before-Start, then cancellation with no unwanted return. No input is
  injected into another computer. The LAN probe independently verifies automatic
  rediscovery of listener/initiator restarts and a broken video channel.
- This release does not adjust the encoder pacing/recovery algorithm. test-host-b slowdown
  evidence remains in local diagnostics; no claim that its FPS issue is fixed.

## Alpha 18 local live diagnostics

- Debug overlay also enables a local machine-readable performance feed. A dedicated
  writer saves an atomic latest JSON snapshot once per second and on session/state
  changes, plus bounded JSONL history (2 MiB current, 2 MiB previous). No web server
  or new listening port is added. No image, clipboard or input-event data is stored.
- Independent view/host records include peer/session identity, dimensions, timestamps,
  cap reason, source and viewer counters, component costs and sample ages. Stale
  peer/session reports are ignored. Disconnected, disabled and stopped states are
  explicit; clearing or replacing a session clears its previous statistics.
- Source values retain their existing measurement meaning: non-IDR encode EMA,
  CPU-call wall times, rolling frame rates, successful Present submissions rather
  than scanout. A pressure bit describes the cap reason and can remain latched
  during recovery. Sample ages must be checked before treating values as current.
- Eight CTest checks pass, including asynchronous file output, JSON escaping,
  session isolation, state transitions and bounded rotation. The source monitor
  script follows a bounded interval and labels a stalled producer. Packaging and
  installed end-to-end monitoring are checked separately.
- test-host-b on alpha.17 already sends the encode details; only the computer where the
  diagnostics are read needs alpha.18. This instrumentation does not change pacing.

## Alpha 17 peer restart discovery and encoder timing

- Replaced the permanent attempted-peer set with a 3-second retry cooldown, driven
  by new LAN announcements. Both encrypted channels must be healthy to suppress
  a retry. Closing either channel closes/removes its whole pair; delayed callbacks
  from an older pair cannot tear down a replacement connection. Replacing a closed
  control connection discards its old video channel before accepting a new one.
- The extended real broadcast discovery test failed before this fix when the
  listener restarted while the initiator stayed up. After the fix, independent
  listener and initiator restarts reconnect without a manual address. The probe
  also tears down only the video channel and requires both channels to recover.
  This restores device connectivity; it does not automatically resume a desktop
  stream or add sleep/secure-session recovery.
- Source diagnostics now split the existing total encode wall time into color/
  synchronization and NVENC/bitstream-wait costs, using the same non-IDR EMA.
  Sample age identifies old estimates; no change to encoder capacity limits is
  made without evidence from the affected test-host-b machine. The reported 40 FPS encoder
  limit remains an open hardware-session diagnosis, not a proven fixed issue.
- Feature negotiation gates the optional timing fields: old peers receive the
  original diagnostic payload. Transport tests round-trip both layouts; the real
  NVENC/NVDEC feedback-delay probe receives detailed timing reports and retains
  its 59.94 FPS cap. Seven CTest checks pass. The 420 x 290 DIP HUD passed native
  visibility/input-transparency checks and was visually reviewed.

## Alpha 16 false network-pressure correction

- Removed feedback-transit excess as a congestion trigger. This measurement
  includes encoding, broker forwarding and control-path scheduling; an absolute
  increase over the session minimum is not evidence of a congested video sender.
  It remains visible in diagnostics. Send readiness is now sampled before the
  pacing decision, so a cleared wait cannot cause another network reduction.
  Sustained send backpressure, decoder queue pressure and measured encode/decode
  capacity still reduce pacing. QP18, 4:4:4 and physical refresh are unchanged.
- Reproduced the defect before fixing it with HiveDeskLoopback --feedback-delay:
  inject 35 ms into feedback delivery after the first second, while video remains
  flowing and feedback reports healthy decoder capacity. The previous implementation
  reduced a 59.94 FPS target to a 1 FPS cap. With the fix, the same test retained
  a 59.94 FPS cap and measured 51.9 received/decoded/presented FPS, with zero rejected
  frames on this machine. Actual frame counts depend on captured desktop changes.
- Seven CTest checks pass, including sustained versus cleared send-wait cases.
  The existing real automatic loopback still reduces under injected decoder pressure,
  emits a recovery IDR, rejects stale sessions and recovers upward without changing
  the physical display mode. These are local regressions, not two-machine LAN
  acceptance or a guarantee that every low-FPS condition has been resolved.
- The pacing fix executes on the controlled computer. Upgrade that computer to
  alpha.16; upgrading only its viewer does not replace the old source controller.

## Alpha 15 immediate hot-corner activation

- Removed the 100 ms corner dwell. The first sample inside the existing 3 DIP
  primary-screen corner starts the preview fade; the sampling timer remains 16 ms.
  Windows scheduling can delay timer delivery; this is not a latency guarantee.
  Fade durations, panel size, expansion and pointer-leave dismissal are unchanged.
- The existing physical-pointer gesture probe now requires the preview to be
  visible immediately after its first corner sample, before waiting for the fade.

## Alpha 14 live stream diagnostics

- Saved Debug overlay toggle defaults off; available in WinUI and the native fallback.
  It displays only over the foreground remote viewer and takes neither focus nor input.
  Fixed settings dimensions and the adaptive pacing policy remain unchanged.
- Viewer counters use approximately one-second windows; the HUD refreshes at most
  four times per second. Discard/skip counters are cumulative per viewing session.
  Decode timing excludes the synchronous render callback; Render and Present are
  CPU-call wall times, not GPU timestamps. Service p90 retains the adaptive loop
  window of up to 64 non-keyframes. Present FPS counts successful API submissions,
  not scanout, and an unchanged desktop legitimately sends fewer frames.
- Source reports arrive about once per second through the broker and LAN transport.
  Encode is the pacer EMA; Capture wait includes capture idle waiting. Send wait is
  backpressure waiting per second. Transit excess is the existing feedback-based
  pipeline round trip above its minimum, not ping or end-to-end visual latency.
  Feedback age, report age and stale markers distinguish missing source data.
- Diagnostics use a negotiated feature bit and a separate bounded message; old
  stream health framing is unchanged. Both endpoints need alpha.14 for source
  detail; the local viewer counters also work with older compatible hosts.
- Transport tests check diagnostic round trips and invalid values. A real automatic
  NVENC/NVDEC loopback received eight source reports and nonzero rolling RX/decode/
  Present rates; its deliberately injected decoder pressure and recovery passed.
  This test does not reproduce or resolve the reported low-FPS condition.
- Synthetic native HUD checks passed: visible, fixed 420 x 258 DIP bounds,
  nonactivating, pointer-transparent, hidden after local return. Rendered HUD and
  real fixed-size light/dark settings were visually inspected at 96 DPI.

## Alpha 13 application identity and release packaging

- Original blue perspective-disc mark is shared by the native agent/tray,
  WinUI executable/window/title bar, installer, Start shortcut and uninstall entry.
  ICO includes 16, 20, 24, 32, 40, 48, 64, 96, 128 and 256 px images; the tray loads
  the small system-size image directly. SVG and PNG sources are in resources;
  scripts/make-icon.py reproduces exports with Pillow (not a runtime dependency).
- Settings uses a fixed 580 x 700 logical-pixel client area, scaled for the
  window's DPI and capped to the work area on small displays. Resize and maximize
  are disabled, minimize remains enabled, and overflow remains scrollable.
  The Win32 fallback already uses a fixed-size window.
- Real light and dark WinUI checks passed. Native window styles and UI Automation
  confirm no resize/maximize, minimize available, and Save visible without scrolling
  at the test machine's 96 DPI. Both rendered windows were visually reviewed.
- Setup uses the product icon and Windows version metadata and offers an optional
  desktop shortcut. The normal Start shortcut remains. Build -Package creates a
  clean versioned handoff folder containing this release's installer, Chinese
  installation/upgrade guide and SHA-256 checksum; full file ZIP remains separate.
- Remote protocols, codecs, broker permissions and Windows authentication are
  unchanged from alpha.12. Long-duration use and lock-screen acceptance are not
  established by this UI/packaging change.

## Alpha 12 local desktop broker

- Capture and SendInput move into HiveDeskHost.exe, a LocalSystem process in the
  signed-in console session, supervised by the HiveDeskAgent Windows service.
  The tray, LAN transport and WinUI settings remain normal-user processes.
  The broker has no listening sockets. It only accepts a local session-specific
  named pipe owned by SYSTEM, with a SYSTEM/current-user ACL and remote clients
  rejected. Both ends check the other process's executable path; the host also
  checks the caller's session. The installed directory must remain admin-writable.
- A fresh capture worker attaches to the input desktop before constructing DXGI.
  Desktop changes / duplication loss rebuild this worker with a fresh IDR while
  keeping the network session. Input switches its separate thread's desktop and
  releases tracked keys/buttons. Pipe closure also releases held input.
- Pipe messages have bounded lengths and input validation. Overlapped I/O has
  cancellable reads/writes, including a stalled video consumer. Frame credits
  propagate network backpressure to the existing automatic encoder pacer.
- A clipboard handoff received on a locked/busy desktop remains cached and retries
  after return to Default. It no longer blocks login input. Broker logs contain
  desktop readiness/errors, never keystrokes or clipboard contents.
- Setup installs the broker independently of the user's login-startup preference.
  Service shutdown waits for its child before upgrading files. ZIP-only hosting
  is no longer supported. Alpha.11 viewers are wire-compatible; the controlled
  machine needs alpha.12 installed.
- Seven CTest checks passed, including malformed broker input/framing and idle
  read / blocked-write cancellation. Real WinUI light/dark/narrow probes passed.
- Installed service runs as LocalSystem with its host in console session 1.
  The ordinary-user LAN agent produced frames through the broker, responded to
  decoder pressure, emitted recovery IDRs and stopped on release. A separate
  local executable was rejected by the pipe identity check.
- HiveDesk.exe --host-check is an explicit installed-broker integration probe.
  It consumes marked synthetic input in test hooks before applications receive
  it, checks keyboard/button injection plus explicit and pipe-close releases,
  and writes PASS/FAIL to the ordinary user log. It does not change the clipboard.
- Actual lock-screen unlock, secure-desktop transition and UAC interaction remain
  user acceptance checks; the development run does not lock the user's computer.
  Password/PIN validation remains Windows-owned. No secure-attention Ctrl+Alt+Del
  generation, pre-login access, logoff/reconnect or multi-session support is added.

## Alpha 11 automatic LAN and desktop preferences

- Settings now has exactly one editable text field (computer name), login-startup
  and tray-icon toggles, and a shortcut recorder. No key, IP, performance or
  relative-mouse settings remain. The existing tray game-mouse action is retained.
- Protocol v2 uses ephemeral CNG ECDH P-256 with SHA-256 derivation. The handshake
  transcript binds both hellos and public keys; directional AES-GCM keys and
  confirmation proofs are derived separately. The video handshake is additionally
  bound to the established control connection's secret. Invalid public-key blobs
  and protocol-v1 peers are rejected. There is no preapproved peer identity: all
  on-link HiveDesk devices are trusted, including a possible active impersonator.
- Discovery needs no key and broadcasts to active IPv4 interfaces' directed
  broadcast addresses. TCP and discovery reject addresses outside local prefixes
  (loopback is allowed for diagnostics). Private/Domain local-subnet firewall rules
  remain. There is no interface selector, manual address or relay fallback.
- Saved legacy protected keys are ignored and left intact for rollback. They are
  never used or exposed by the new application. Both endpoints must upgrade.
- A live Rename control message updates the peer name without stopping media or
  restarting discovery. The transport retains the old keyed path for regression
  tests only; the application always selects keyless LAN mode.
- Startup now uses HKCU\Software\Microsoft\Windows\CurrentVersion\Run with
  --background. Disabled startup exits on that route; a normal manual launch
  always opens Settings, including an already-running tray-hidden instance.
  Installer upgrade removes the old service and launches the current-user agent.
- Ctrl+Alt+H is the default Show Hive shortcut, separate from the physical-backtick
  escape chord. Global registration rejects conflicts, and a new shortcut is
  reserved before the old one is released. Remote capture handles the chord
  locally and releases forwarded modifiers. Shortcut recording suspends the old
  registration temporarily and restores it on finish/close. Local runtime conflict
  is shown when Settings opens. No-repeat registration prevents repeated toggles.
- Tray double-click opens Settings; single-click is delayed to distinguish the
  double-click. Explorer recreation respects the hidden preference. Hotkey/tray
  summoned lists stay open without requiring the pointer to arrive within 250 ms;
  corner-triggered lists retain their original dismissal behavior.
- Six CTest checks and light/dark/narrow real WinUI probes passed. Added actual
  ECDH key agreement, keyless dual-channel payload, invalid public key, legacy
  isolation and live rename coverage. Real interface discovery created both
  encrypted channels without connect() or IP arguments. The 1440p automatic video
  loopback delivered 249/249 frames with no sender rejection and retained adaptive
  overload/recovery behavior.
- Real agent + WinUI automation verified startup entry removal, tray removal,
  reopening Settings without another agent, disabled background launch exit,
  manual launch despite disabled startup, and restoration of original preferences.
  Real Ctrl+Alt+J recording, occupied-shortcut rejection without persistence,
  successful registration and release of the previous shortcut all passed.
- Native shell probe passed persistent hotkey list/toggle dismissal and actual tray
  double-click Settings launch, plus snapshot/cursor/gesture regressions. Windows
  denied foreground during this run, so badge visibility and cached-view pixel
  checks were skipped; those passed in alpha.10 and their implementation is unchanged.
- Reboot/login execution and two-machine alpha.11 interoperability still require
  real deployment validation. Local loopback is not a LAN latency measurement.

## Alpha 10 automatic quality and refresh-aware pacing

- Both peers advertise an authenticated automatic-stream capability. The viewer
  sends its primary refresh in millihertz and a session ID; the source targets
  min(source, viewer). QueryDisplayConfig/EnumDisplaySettings are read-only.
  No SetDisplayConfig/ChangeDisplaySettings call exists in the application.
  Legacy Start/Frame messages remain supported; old peers keep legacy CBR.
- Automatic NVENC uses HEVC 8-bit 4:4:4, full-range BT.709, constant QP18,
  disabled AQ/lookahead/multipass, no B frames and no periodic IDR. Startup and
  decoder recovery request an IDR. Quality is fixed; it is not lossless and cannot
  guarantee absence of all compression artifacts for arbitrary content.
- A 100 ms viewer feedback interval reports decoder/conversion/presentation
  service time, queue age and recovery requests. Service uses a 64-sample p90,
  excluding cold-start IDRs. Source pacing reserves 10% processing headroom,
  reduces promptly on pressure, and probes upward after a healthy interval.
  Obsolete pictures still decode for reference correctness but can skip rendering.
  A bounded queue requests a new IDR after overload; requests are throttled.
- Network pressure uses the bounded sender slot and echoed source timestamps,
  subtracting the receiver's local arrival age. This avoids assuming synchronized
  computer clocks. Excess above the running transit baseline is a congestion
  signal, not a one-way latency measurement. No bandwidth/QP quality oscillation.
- Health messages carry session IDs. The badge evaluates negotiated versus
  current pacing cap, not the number of frames in a static desktop. Below 80%
  for one second shows Device busy / Slow connection; three healthy seconds
  clear it. It is 136x30 DIP, non-activating, click-through, and only shown over
  the foreground remote viewer. Returning locally resets it.
- Normal WinUI and fallback settings no longer expose FPS or bitrate. Legacy
  saved fields remain only for storage/IPC compatibility.
- Hardware codec test passed for the automatic mode: parsed full-range 4:4:4,
  one-pixel alternating chroma and bars, 4/4 frames; RGB MAE 0.430, max 7.
  This small test image does not establish perceptual quality for every workload.
- Real 2560x1440 local capture -> encrypted TCP -> NVDEC/D3D11 loopback passed:
  249/249 frames, zero sender rejections. Injected decode pressure reduced a
  negotiated 59.940 Hz target to 15 fps; it recovered to 20 fps in the bounded
  test. Recovery IDR and stale-session rejection passed. Physical primary refresh
  remained 99.946 Hz. This is a deterministic overload test, not a LAN benchmark.
- Native shell probe passed actual badge onset, size, no focus/input interception,
  healthy clearing, plus existing snapshots/cursor/compositor/gesture checks.
- Both-end alpha.10 real-LAN workload and older-driver validation remain pending;
  local checks do not establish sustained high-refresh performance on the other PC.

## Alpha 9 snapshots and native cursor synchronization

- Each switcher opening requests one JPEG snapshot from each capable online peer.
  Expanding the same opening does not request again. There is no refresh timer.
  Requests execute off the control reader/UI; pending captures are coalesced per
  peer, and simultaneous requests can reuse a capture for 250 ms. Replies are
  correlated by request ID; stale/unsolicited replies do not replace the cache.
  Only the primary screen is sampled, aspect-fitted into 672x378; JPEG is capped
  at 256 KiB. Decoder verifies JPEG format and dimensions before pixel allocation.
  A fresh preview fades in over 180 ms; cached images appear immediately.
- Cursor shape, alpha, AND/XOR masks, hotspot, visibility and host-side position
  changes use authenticated control messages. Only active hosting samples the
  cursor, on the existing 16 ms UI timer; unchanged states send nothing. Native
  cursors render locally, independent of video rate. Injected controller motion is
  not echoed back as pointer positioning. Sidebar/settings interaction and local
  movement in the last 100 ms take precedence. Relative mode retains hidden cursor.
- Feature support is negotiated in the authenticated opaque challenge: an 8-byte
  tag, 4-byte capability word and 20 fresh random bytes preserve the existing
  32-byte handshake layout. Old peers keep normal video/input; no auxiliary packet
  is sent to a peer without the feature flag. Both ends need alpha.9 for new features.
- JPEG/native cursor checks and real transport tests cover idle snapshot exchange,
  request correlation, cursor shape and hide-only messages, malformed sizes,
  capability detection and legacy challenges. Existing codec/color and WinUI checks
  pass. A real primary screenshot probe checks a known colored window and saves no
  image. Native switcher tests check a never-viewed peer's preview, one request per
  opening, refresh on reopen, and the existing corner/fade/reversal interactions.
- Actual native viewer checks passed for remote I-beam pixels, hide/show, unrelated
  peer rejection, host position mapping and absence of input echo. The real gesture
  run observed 12 list-fade shades and 7 dismissal shades, with reversal intact.
  Pixel assertions suspend the pointer-leave timer; the gesture phase re-enables it.
  Cached-last-view verification was skipped when Windows denied foreground focus;
  the fresh never-viewed-peer snapshot assertion did run and pass.
- Against the running alpha.9 agent, the authenticated loopback probe received and
  decoded two requested snapshots plus the newest of 20 rapid requests, with zero
  video/cursor messages while idle. Start then delivered three actual video frames
  and reconstructable native cursor updates;
  Stop ended both. No keyboard/mouse injection, clipboard writes or saved images.
  A real alpha.9-to-alpha.9 two-PC feature check remains for user validation. The
  existing alpha.8 test-host-b peer still authenticated successfully with the new agent.
- Alpha.8 follow-up: the user confirmed real two-PC viewing in both directions
  between test-host-a and test-host-b after a disconnected control pair was re-established.
  test-host-b's supplied GPU/driver was RTX 2080 SUPER / 577.00. Early R500 hardware testing
  remains pending; alpha.9 leaves the driver-only codec path unchanged.

## Alpha 8 R500-era driver compatibility

- Remote evidence identified the actual failure on test-host-b: two RTX 2080 SUPER
  GPUs on driver 577.00; CUDA 13 runtime initialization failed during GPU scratch
  allocation. Prior connection/Network Key authentication had succeeded.
- Removed CUDA runtime linkage, NVCC and the CUDA Toolkit build dependency.
  Color conversion now uses stable CUDA driver functions and original PTX 6.4
  kernels targeting sm_75. Lowered NVENC/NVDEC headers and structure versions to
  API 11.1.5 (nv-codec-headers n11.1.5.3), whose documented Windows minimum is
  471.41. This targets early R500 drivers without changing HEVC 4:4:4 to 4:2:0.
- Transport, hardware codec and new GPU-color reference checks passed. Random
  67x19 and 257x33 images verify both color directions against CPU BT.709 within
  one code value, padded planes, untouched guard bytes, scratch resizing and alpha.
- On RTX 5060 / driver 595.79: actual 2560x1440 desktop capture/HEVC444 roundtrip
  passed (RGB MAE 0.470), hidden renderer passed, and authenticated full-media
  loopback delivered 119/119 frames with no backpressure. Synthetic fixed-texture
  throughput was 175.7 fps / 5.7 ms per serialized frame; not remote latency.
- R500 compatibility is based on API/ISA requirements, not an old-driver hardware
  run. This workstation cannot test a R500 driver on its RTX 5060. RTX 2080 SUPER /
  577.00 was subsequently user-validated in both directions (see alpha.9 follow-up);
  actual early-R500 hardware still requires validation.

## Alpha 7 live two-PC diagnosis and connection feedback

- Reproduced authentication and both TCP channels to test-host-b at the peer LAN address.
  A desktop request timed out with zero received video frames. Remote capture/
  encoding is not yet diagnosed; this is not a successful two-PC desktop test.
- Fixed Connect by IP attempting a duplicate handshake after automatic discovery
  already connected the peer. Outbound attempts are serialized and an established
  authenticated control/video pair is reused. Regression tests exercise both
  directions of reuse, followed by actual clipboard/input/frame delivery.
- Viewer readiness now uses successful frame presentation, not a search for the
  word NVDEC in a status/error string. Specific decoder errors are retained;
  timeouts distinguish zero incoming video from decode/presentation failure.
  Explicit connection failures show an error dialog. Post-start host exceptions
  are written to the normal log, not only debugger output.
- Local transport/codec and light/dark WinUI checks passed. Capture/encode/network/
  decode/presentation loopback passed with 119/119 frames, no backpressure. After
  the duplicate-connection fix, a real peer request reached the video timeout
  instead of failing authentication. Remote-side evidence is still required.

## Alpha 6 short, visible Network Key

- Network settings now start with a plain TextBox for Network Key, optional Copy,
  and a same-key/autoconnect hint. Enter saves from the key field. Manual IP
  connection is collapsed by default. Native fallback settings follow the same
  visible-key validation and preserve letter case.
- New installations generate 8 characters with system randomness and unbiased
  sampling. Accepts 4-64 printable ASCII characters without spaces; short keys are
  case-sensitive. PBKDF2-HMAC-SHA256 (100,000 rounds, application-specific salt)
  derives transport key material at network startup. DPAPI storage and encrypted
  transport remain. Legacy 64-hex values decode unchanged; upgrades do not silently
  rotate them. Short keys require alpha.6 or newer on both peers.
- Transport checks cover a .NET-computed derivation reference vector, legacy key
  bytes, case differences, invalid keys and actual short-key loopback authentication.
  WinUI light/dark probes cover visible values, mixed case, a four-character key,
  invalid values, saving, FPS=0 and narrow layout.

## Alpha 5 opacity transitions

- Replaced horizontal slides and animated expansion clipping with DirectComposition
  opacity effects. Entry fades in over 240 ms; pointer-leave fades out over 200 ms.
  Expansion retains the preview area and fades in the additional list area over
  240 ms. Bounds remain fixed throughout each transition. Re-entry resumes from
  the current opacity; Escape/selection still dismiss immediately. Windows' animation
  preference is respected. Timing remains an approximation, not extracted Win8 data.
- Actual compositor samples over a uniform test backdrop passed with UI dispatch
  blocked: 10 intermediate opacity shades with both horizontal edges synchronized.
  Real pointer corner-to-list route passed: 12 expansion shades, 7 dismissal shades,
  completed dismissal and mid-fade reversal. The test restores the pointer afterward.
- Cross-thread peer updates, selection, return and capture release passed. This run
  skipped the thumbnail pixel assertion because the viewer did not acquire foreground.

## Alpha 4 WinUI 3 settings

- Replaced the packaged settings UI with actual WinUI 3 PasswordBox, NumberBox,
  ToggleSwitch, InfoBar and standard buttons. System light/dark resources and
  accent are used; Windows 11 enables Mica. The native switcher is unchanged.
- Bundles .NET 10, the Windows App SDK WinUI component and app-local VC runtime.
  Build and packaging are local. No hosted CI or target-machine SDK is needed.
- Release build and transport/codec CTests passed. Real WinUI self-tests passed
  in both light and dark themes: FPS=0 persistence in preview, invalid-key
  rejection, toggle and 500x600 layout. Full-size light/dark screenshots inspected.
- Actual settings-to-agent IPC verified through UI Automation: Save confirmed
  against persisted settings and relative mouse toggled on/off, then restored.
  Secrets are absent from process arguments; settings continue to use user DPAPI.
- First-instance and existing-instance --settings launch verified. Opening
  Settings returns input locally. Native settings remain a development fallback
  only when the separately packaged helper is absent.
- Tested on Windows 10 Pro build 19045. Windows 11 Mica, high-DPI layout and a
  clean second-PC installation have not been validated on this machine.

## Alpha 3 compact switcher and actual gesture transitions

- Rail width reduced from 360 to 200 DIP; previews are 176x99 DIP with 13 DIP
  machine-name text. Win8 reference screenshots were inspected for the compact
  rail proportions; these dimensions are an approximation, not an extracted OS metric.
- Fixed the real corner-to-list path overwriting the in-flight entry animation
  with its final offset. Expansion now animates a composition clip from the small
  preview height to the list height; duplicate show requests no longer reset motion.
- Added animated pointer-leave dismissal and reversal on re-entry. Explicit
  desktop selection and the escape chord retain immediate dismissal.
- Native --gestures probe temporarily moves/restores the pointer and exercises
  the actual corner timer route. Passed: entry continues with UI dispatch blocked;
  expansion samples grew from 259 to 1440 pixels across ten distinct extents;
  dismissal shrank from 200 to 0 pixels across eight distinct extents. This is
  evidence of intermediate frames, not a frame-rate measurement.
- Native settings/selection/return smoke passed; synthetic previews visually
  inspected at the reduced width. The normal smoke skipped its foreground-dependent
  pixel assertion when Windows did not grant the demo viewer foreground focus.

## Alpha 2 switcher update

- Large 16:9 preview panes with machine-name corner badges only. Windows DWM
  colorization supplies accent colors; theme/color change notifications refresh
  the visible panel. Settings remain in the tray/sidebar context menu.
- A retained DirectComposition surface replaces timer-driven HWND movement.
  The 180 ms ease-out slide is evaluated by DWM and respects the Windows
  client-area animation preference. Content uploads occur on UI changes, not
  every animation frame. This does not claim a measured refresh rate.
- Native smoke verified at least eight different visible positions while the
  UI thread was deliberately blocked for 260 ms, with the HWND fixed at x=0.
  Settings, selection, return and mouse release passed. Synthetic cached
  previews and the actual current Windows accent were inspected in a screenshot.
- The encoder configuration is unchanged: CBR target, no filler padding.

Validated on 2026-10-25 on Windows x64, NVIDIA RTX 5060, driver 595.79, primary
display 2560x1440. Local MSVC Release build, CUDA Toolkit 13.0.3, public codec API
headers 13.0. No hosted CI was used.

## Completed checks

- Local Release compilation and transport/codec CTest checks passed. Transport
  probes use real loopback sockets and verify pairing, wrong-key refusal, encrypted
  message tampering, ordered clipboard/input delivery, frame metadata and shutdown
  control-message draining.
- HEVC 4:4:4 NVENC -> NVDEC synthetic color/chroma test passed on actual hardware.
  This is not a mocked encoder or a 4:2:0 fallback.
- Actual primary-display capture -> CUDA/D3D11 conversion -> NVENC444 -> NVDEC ->
  test readback passed. A temporary four-color test window was verified in the
  captured and decoded pixels, then closed. Latest observed run: 2560x1440,
  397488-byte IDR, RGB mean absolute error 0.479. The 46 ms single-frame test time
  includes setup/readback and is not remote interaction latency.
- Hidden-window GPU rendering passed: decoded color quadrants, orientation,
  letterboxing at 512x512 and resizing/pillarboxing at 640x360.
- Same-machine full media/network loopback passed: real primary capture,
  authenticated encrypted TCP transport, decoding and hidden HWND presentation.
  Latest run delivered 120 of 120 encoded frames without backpressure;
  53.1 fps included startup at a requested 60 fps. This does not measure LAN latency.
- Fixed-texture 1440p GPU codec/render throughput: 120/120 measured frames,
  174.3 fps / 5.7 ms mean serialized processing per frame after eight warmups.
  This excludes acquisition, network, input and scanout, and is not general-content
  performance or remote input latency.
- Native shell smoke passed: masked pairing key, Copy key button presence, FPS=0
  settings round-trip, cross-thread UI updates, peer selection, viewer return,
  mouse release, and actual cached synthetic thumbnail pixels. A separate native
  Switcher screenshot was inspected. Demo peers were synthetic, not paired machines.
  Hidden-start shell smoke also passed after fixing the initial window-show override;
  that run skipped thumbnail validation because it could not take foreground focus.
- Release executable dependency inspection found Windows DLLs, without a CUDA
  Toolkit DLL or Visual C++ redistributable dependency. NVIDIA driver DLLs are
  dynamically loaded. CUDA runtime and MSVC runtime are statically linked.
- Setup.exe installed successfully on this computer, with matching installed and
  built executable SHA256. HiveDeskAgent was verified Running/Auto in Session 0;
  the service-launched tray process was in user Session 1 with TCP 47980 and UDP
  47979 listening. Both program-scoped inbound rules were read back as enabled,
  restricted to Private/Domain and LocalSubnet. Normal Quit and a fresh launch
  passed. A full Windows reboot has not been performed.

## Limits and remaining acceptance

- Two-PC discovery/authentication was exercised with test-host-b, but desktop
  delivery failed with zero video frames. Remote capture/encoding, bidirectional
  clipboard handoff, physical keyboard/mouse routing and end-to-end latency still
  require acceptance. Parsec-level performance is a target, not a measured claim.
- Physical Ctrl+Alt+backtick must be verified by the user with a real keyboard.
  The shell probe checks the return/cleanup path without synthesizing physical
  input or changing the user's text clipboard.
- Alpha.10 negotiates the lower primary refresh and adapts downward to processing
  and transport capacity. Legacy FPS=0 disables extra application pacing only.
  Rendering cannot create unique desktop frames above the source refresh.
- Both computers need compatible NVIDIA HEVC444 hardware, an R500-or-newer driver
  (alpha.8 compatibility target; old-driver hardware validation pending), an NVIDIA
  attached primary display, and an interactive signed-in desktop. SDR only;
  HDR PQ and rotated displays are rejected. No software codec fallback.
- Alpha.12 implements a SYSTEM capture/input path for the signed-in session
  and lock desktop. Actual lock/unlock and UAC acceptance remain pending.
  Ctrl+Alt+Del generation and pre-login remote desktop are not implemented.
- Native cursor shapes up to 256x256 synchronize on alpha.9 peers. Animated cursor
  frames are not individually streamed; application-drawn cursors remain in video.
- Switcher thumbnails refresh once per opening on alpha.9 peers; older peers retain
  last-view caches. Previews do not continuously stream or open a control session.
- No audio, multiple monitors, dedicated sleep/resume handling,
  IME special handling or nesting prevention, as requested.
- Text replaces the destination clipboard when switching. Images and files are
  ignored. Transport accepts at most 1 MiB of UTF-16 text per clipboard handoff;
  larger clipboard text prevents that handoff. In alpha.11 every on-link HiveDesk
  peer can request control; no shared pairing key or approval step is used.
- The installer is an unsigned alpha. Private/Domain local-subnet firewall rules
  do not open access on Public networks. Signing, reboot validation and extended
  operation are not established by the local component tests.

## Reproduce

Run scripts/build.ps1 for Release build plus CTest. Explicit probes are
HiveDeskCheck.exe --capture, --render and --throughput; the throughput probe uses
a fixed GPU texture and excludes capture, network, input and scanout latency.
Development build also includes HiveDeskLoopback.exe and HiveDeskShellSmoke.exe.
Run scripts/build.ps1 -Package to produce the portable ZIP, Inno installer and
SHA256SUMS.txt entirely on the local machine.
