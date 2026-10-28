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

An exact query is NOT a functioning backend. Remaining Intel work includes the
D3D11 texture allocator, color conversion, asynchronous encode/decode ownership,
bitstream/chroma verification and compatibility negotiation. AMD needs a
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
