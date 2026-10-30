# Media backend bring-up

Production hosting uses NVIDIA NVENC / CUDA, or the Intel hardware HEVC backend
added in 0.14.0 when the primary display belongs to an Intel adapter. Receiving prefers NVIDIA
NVDEC and otherwise uses the existing CPU HEVC 4:4:4 decoder plus D3D11
presentation, including on Intel/AMD adapters. The Intel dispatcher is linked
statically but its hardware session is created only for an Intel host. NVIDIA
keeps its existing encoder implementation; no silent chroma downgrade is negotiated.

The Intel host converts BGRA to full-range BT.709 AYUV on D3D11, reads back into
bounded system-memory surfaces, then submits to the hardware encoder. It uses
two application IO slots, no frame reordering, CBR and the existing QP 16..22
quality policy. Since 0.14.1 codec AsyncDepth is 1: JT's Media SDK 1.35 retained
the first picture with depth 2, which cannot support a one-picture warm stream.
Application conversion/completion/delivery remain separate stages.
It shares the existing latest-capture scheduling, independent output/delivery,
warm/Peek lifecycle and adaptive pacing. Each requested recovery IDR is checked
against its actual SPS before delivery. Timestamp correspondence is mandatory.
Supported runtime rate changes request continuation of the coded sequence;
unsupported reset queries retain the current codec and adaptive pacing.
Since 0.14.2, capture pacing changes never rewrite the Intel codec's sequence
frame rate. Only the bitrate is proposed for a reference-preserving reset.
If Reset rejects a successful Query, the backend reads back the original rate,
timing, format and quality before disabling live resets for that session;
device errors or changed parameters still fail. Subsequent generated pictures
must retain their sequence, colors and forced-IDR behavior in qualification.

This first Intel path has upload/readback overhead; it is not zero-copy.
Intel hardware receiving remains separate work. AMD hosting is not implemented.

`HiveDeskMediaProbe.exe` is a separate, explicit diagnostic executable:

- Enumerates real DXGI adapters with vendor/device IDs and LUIDs.
- On Intel, uses the pinned official VPL dispatcher, restricts selection to
  hardware D3D11 on that DXGI adapter, binds the device, and queries HEVC REXT
  AYUV 8-bit 4:4:4 encode/decode. Encode requests one-frame async depth, no B
  frames, and CBR. Reports the raw query status and actual output parameters;
  software/partial acceleration or modified chroma is not accepted as exact.
- On AMD, loads the driver's AMF runtime from System32 only, binds that
  adapter's D3D11 device, and reports HEVC acceleration, maximum profile and
  accepted input formats separately. The pinned AMF HEVC interface exposes
  Main/Main10; RGB/BGRA input is not evidence of a 4:4:4 encoded bitstream.
- Does not capture the desktop, inject input, create a network session, alter
  GPU settings or persist a backend selection. Missing driver runtimes are
  reported per adapter, without disrupting enumeration of other adapters.

`HiveDeskMediaProbe.exe --intel-roundtrip` additionally runs a generated-content
Intel hardware qualification test. It requires exact low-delay HEVC REXT/AYUV
8-bit 4:4:4 initialization. VPL 2.x uses managed D3D11 surfaces. Since 0.13.6,
Media SDK 1.x hardware sessions use bounded application-owned AYUV system-memory
surfaces with runtime upload/readback. Locked surfaces are not reused, and
storage outlives codec shutdown. Since 0.13.7, encoded output capacity follows
the initialized BufferSizeInKB and BRCParamMultiplier, bounded to 64 MiB.
Sixteen 640x360 patterns with single-pixel chroma changes pass through encoding,
asynchronous completion, encoder drain, SPS inspection, hardware decode and
mapped output comparison. The actual bitstream must declare 4:4:4, 8-bit and
the expected dimensions; all sixteen decoded frames must match the generated
sequence within the documented mean absolute component error bound of 12.
The test reports encoded bytes and Y/U/V mean/maximum errors. Exit status is
0 for passing Intel adapters, 1 for a failed test, and 2 when Intel hardware is
absent. This test uses CPU-mapped diagnostic patterns, not desktop capture or
a production zero-copy path, and does not qualify latency or frame throughput.

An exact query or generated roundtrip is NOT a functioning backend.
Remaining Intel hardware work includes hardware decoder integration and
real-device sustained throughput/latency qualification. The production host
uses the system-memory path even on newer runtimes; managed surfaces remain
an isolated probe path. AMD needs a
verified full-chroma representation first; no silent 4:2:0 fallback is allowed.
Dual-stream representations require separate interoperability and latency work.

Bring-up on each candidate device must record the full probe output, driver
version, primary-display adapter, and round-trip decoded color test. Do not infer
support from a marketing family or from another GPU's results.

## Remote qualification

Since 0.13.5 the installed agent can run fixed diagnostic jobs over its existing
authenticated transport. No remote shell, arbitrary executable, file-reading,
input or clipboard command is exposed:

```powershell
& "$env:ProgramFiles\HiveDesk\HiveDeskRemoteMediaProbe.exe" <IPv4> capabilities
& "$env:ProgramFiles\HiveDesk\HiveDeskRemoteMediaProbe.exe" <IPv4> intel-roundtrip
& "$env:ProgramFiles\HiveDesk\HiveDeskRemoteMediaProbe.exe" <IPv4> receive
& "$env:ProgramFiles\HiveDesk\HiveDeskRemoteMediaProbe.exe" <IPv4> host
```

`receive` displays a read-only preview of the requesting computer on the target
for 15 seconds, using the production stream/decoder. The source must already be
running HiveDesk. It uses a warm preview lease and cannot replace an active
controller. This is a connectivity/timing diagnostic, not sustained-motion or
input-latency qualification. Output includes decode, conversion, drawing,
presentation and queue timings. A cancelled or declined run is not a full pass.

`host` (0.14.0+, separately negotiated feature) runs a generated 48-frame Intel
production-backend sequence with software-decoded color verification, two-slot
ownership, forced IDR, live rate changes where supported and final draining.
It then captures the real primary desktop through MediaHost, software-decodes
an initial frame and a requested recovery IDR, and stops. No picture is saved
or transmitted, and no desktop input or test window is created. This fixed
probe is bounded to 30 seconds and is not part of automatic CTest.

Only one job runs at a time, with a 30-second admission cooldown. The agent
requires no active HiveDesk session, Peek or graphics preparation; it cancels
when those conditions change or on disconnection/shutdown, bounds
child-process lifetime and report size, and kills the child job on cancellation.
Old peers without the diagnostic feature are rejected before sending the request.

## Recorded Intel device evidence

On 2026-10-30, JT-WORKS-PRINT-SHOP reported Intel Iris Plus Graphics (8086:8a52),
driver 31.0.101.2130, Media SDK API 1.35, and a 2736x1824 primary display.
Exact HEVC 4:4:4 encode/decode capability queries succeeded. A 0.13.6 read-only
receiving test displayed DEVG's 2560x1440 stream through the CPU decoder: 674
frames received and 640 distinct presentations over 15 seconds. Rates varied;
this does not establish motion smoothness or hardware decoding.

The initial generated hardware test exposed two probe assumptions: requiring
VPL 2.x managed surfaces, then a fixed 4 MiB output allocation that returned
MFX_ERR_NOT_ENOUGH_BUFFER. The corresponding probe fixes are 0.13.6 and 0.13.7.
These failures alone do not establish missing hardware codec support.
The corrected 0.13.7 probe passed on both enumerated Intel adapter instances:
16 encoded and decoded frames, actual HEVC444/8-bit SPS, mean absolute Y/U/V
errors 0.34264/0.326287/0.278204. Its initialized output allocation was
25,000,000 bytes (12,500 KB with multiplier 2). This generated qualification
does not establish desktop-streaming performance; 0.14.0 production-host
acceptance must be recorded separately.

SDK sources and licenses are pinned under `third_party/onevpl` and
`third_party/amf`. No additional vendor runtime is bundled or required by the
existing NVIDIA software.
