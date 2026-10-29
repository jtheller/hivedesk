#!/usr/bin/env bash
set -euo pipefail
root=$(cygpath -u "$1")
export PATH="$root/usr/bin:$root/nasm-2.16.03:$PATH"
mkdir -p "$root/objects" "$root/install"
cd "$root/objects"
../FFmpeg-n9.0.2/configure --toolchain=msvc --arch=x86_64 --prefix="$root/install" \
  --disable-everything --disable-autodetect --disable-programs --disable-doc --disable-debug \
  --disable-network --disable-avdevice --disable-avformat --disable-avfilter \
  --disable-swscale --disable-swresample --disable-static --enable-shared \
  --enable-decoder=hevc --enable-parser=hevc --enable-w32threads
make -j4
make install
