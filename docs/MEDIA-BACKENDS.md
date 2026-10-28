# Media backend bring-up

The production path is still NVIDIA NVENC / NVDEC / CUDA. The agent and host
do not link Intel VPL, load AMF, select a different backend, or negotiate a
different chroma format in this release.

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
Intel hardware qualification test. It requires VPL 2.x and exact low-delay
HEVC REXT/AYUV 8-bit 4:4:4 initialization. Sixteen 640x360 patterns with
single-pixel chroma changes pass through managed D3D11 encode surfaces,
asynchronous completion, encoder drain, SPS inspection, hardware decode and
mapped output comparison. The actual bitstream must declare 4:4:4, 8-bit and
the expected dimensions; all sixteen decoded frames must match the generated
sequence within the documented mean absolute component error bound of 12.
The test reports encoded bytes and Y/U/V mean/maximum errors. Exit status is
0 for passing Intel adapters, 1 for a failed test, and 2 when Intel hardware is
absent. This test uses CPU-mapped diagnostic patterns, not desktop capture or
a production zero-copy path, and does not qualify latency or frame throughput.

An exact query or generated roundtrip is NOT a functioning backend. Remaining
Intel work includes desktop-texture color conversion, asynchronous production
encode/decode ownership, negotiated compatibility, GPU-native presentation and
real-device throughput/latency qualification. Managed surfaces and SPS parsing
now exist in the isolated diagnostic only. AMD needs a
verified full-chroma representation first; no silent 4:2:0 fallback is allowed.
Dual-stream representations require separate interoperability and latency work.

Bring-up on each candidate device must record the full probe output, driver
version, primary-display adapter, and round-trip decoded color test. Do not infer
support from a marketing family or from another GPU's results. This release was
compiled and exercised on NVIDIA hardware only; AMD/Intel hardware results are
not yet available.

SDK sources and licenses are pinned under `third_party/onevpl` and
`third_party/amf`. No additional vendor runtime is bundled or required by the
existing NVIDIA software.
