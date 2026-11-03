#!/bin/sh
# Builds the small static FFmpeg the Windows plugin needs, with MinGW-w64 (Linux or MSYS2):
#   libavformat/libavcodec/libavutil  HEVC/H.264 decoding (D3D11VA and DXVA2 on the GPU) for the plugin
#   ffmpeg.exe, ffprobe.exe          splitting the lenses, frames for the seam fit, the tag movie (ProRes 4444)
# Everything is LGPL; nothing else is linked in.
#
# usage: tools/build_ffmpeg_windows.sh /path/to/ffmpeg-source [prefix]   (FFmpeg 7.1 tested)
set -e
src="$(cd "$1" && pwd)"
prefix="$(mkdir -p "${2:-$(dirname "$0")/../deps/ffwin}" && cd "${2:-$(dirname "$0")/../deps/ffwin}" && pwd)"
build="$prefix-build"
mkdir -p "$build"
cd "$build"
"$src/configure" --prefix="$prefix" \
  --target-os=mingw32 --arch=x86_64 --cross-prefix=x86_64-w64-mingw32- \
  --enable-static --disable-shared --disable-autodetect --enable-w32threads \
  --enable-d3d11va --enable-dxva2 --enable-zlib \
  --disable-everything --disable-doc --disable-network --disable-debug --disable-ffplay \
  --enable-ffmpeg --enable-ffprobe \
  --enable-demuxer=mov,image2,png_pipe --enable-muxer=mov,rawvideo \
  --enable-decoder=hevc,h264,png,rawvideo --enable-encoder=prores_ks,rawvideo,png \
  --enable-parser=hevc,h264,png --enable-protocol=file,pipe \
  --enable-filter=scale,format,null,copy,trim,setpts,fps,aformat,anull,aresample,atrim,asetpts,anullsink,nullsink,buffer,buffersink,abuffer,abuffersink \
  --enable-hwaccel=hevc_d3d11va,hevc_d3d11va2,hevc_dxva2,h264_d3d11va,h264_d3d11va2,h264_dxva2 \
  --extra-ldflags=-static
make -j"$(nproc 2>/dev/null || echo 4)"
make install
echo "FFmpeg for Windows in $prefix"
