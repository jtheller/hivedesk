# NVIDIA codec API headers

The files in nvcodec/include/ffnvcodec are from FFmpeg/nv-codec-headers,
tag n11.1.5.3, commit 2c961f06047c31da56d9e1557ea395f3fef20277.
Source: https://github.com/FFmpeg/nv-codec-headers
These correspond to NVIDIA Video Codec SDK API 11.1.5. Its documented Windows
driver minimum is 471.41, below HiveDesk's R500-series compatibility target.
Each header retains its NVIDIA copyright and MIT-style license notice.
No NVIDIA driver binaries or restricted SDK archive are distributed here.

HiveDesk's color-conversion kernels are original PTX 6.4 targeting sm_75. They
are loaded through the system NVIDIA CUDA driver API and JIT-compiled for the
installed GPU. No CUDA runtime, NVRTC, Toolkit library or driver DLL is bundled.
The build does not require a CUDA Toolkit installation.

# Settings runtime dependencies

Settings uses Microsoft Windows App SDK WinUI, .NET 10 and Windows SDK
projections. Published runtime files preserve their supplied notices, and
NuGet package license/notice files are copied to settings/licenses when built.
The app-local Microsoft Visual C++ 143 runtime comes from the installed Visual
Studio Build Tools redistributable directory. These components retain their
respective Microsoft and third-party licenses; the HiveDesk source license
does not replace them.
