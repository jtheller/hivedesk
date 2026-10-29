# FFmpeg HEVC software decoder

Upstream: https://github.com/FFmpeg/FFmpeg/tree/n9.0.2

Source archive: https://github.com/FFmpeg/FFmpeg/archive/refs/tags/n9.0.2.tar.gz
SHA256: `6e374ed621e48faa40639307dff48ba6fe574a509977956d2cce9669b7cc27e9`

Unmodified upstream source. Built as dynamically linked LGPL 2.1-or-later
avcodec/avutil DLLs with MSVC and NASM. Only HEVC decoding/parsing is enabled;
no encoders, command-line programs, networking, GPL or nonfree components.
CPU SIMD selection remains runtime-dispatched for older x64 CPUs.

The installer includes the exact source archive, COPYING.LGPLv2.1 and build
scripts in licenses/ffmpeg. The DLLs can be replaced with ABI-compatible builds.
Run scripts/build.ps1 from the source repository; it invokes build-codec.ps1
inside the MSVC x64 environment. Git for Windows and 7-Zip are build prerequisites.
build-codec.ps1 pins/checks the source, GNU make and NASM download hashes.

FFmpeg is copyrighted by its respective contributors. This package does not
claim ownership of FFmpeg. See the included source and license for notices.
