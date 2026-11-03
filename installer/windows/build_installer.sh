#!/bin/sh
# Builds the Windows installer: dist/Insta360Stitch-<version>-windows-setup.exe
#
# Needs MinGW-w64, NSIS (makensis) and the static FFmpeg from tools/build_ffmpeg_windows.sh
# (FFMPEG=<its prefix>, default deps/ffwin next to this repository's plugin folder).
set -e
cd "$(dirname "$0")"
here="$(pwd)"
project="$(cd ../.. && pwd)"
plugin="$project/plugin"
version="$(sed -n '/CFBundleShortVersionString/{n;s/.*<string>\(.*\)<\/string>.*/\1/p;}' "$plugin/Info.plist")"
ffmpeg="${FFMPEG:-$project/../../deps/ffwin}"

if [ ! -f "$ffmpeg/lib/libavcodec.a" ] || [ ! -f "$ffmpeg/bin/ffmpeg.exe" ]; then
    echo "no FFmpeg for Windows in $ffmpeg: run tools/build_ffmpeg_windows.sh first (or set FFMPEG)" >&2
    exit 1
fi

make -C "$plugin" -f Makefile.win FFMPEG="$ffmpeg" all strip

stage="$here/build/stage"
rm -rf "$here/build"
mkdir -p "$stage/tools" "$project/dist"
cp -R "$plugin/build-win/Insta360Stitch.ofx.bundle" "$stage/"
res="$stage/Insta360Stitch.ofx.bundle/Contents/Resources"
cp "$ffmpeg/bin/ffmpeg.exe" "$ffmpeg/bin/ffprobe.exe" "$res/"
x86_64-w64-mingw32-strip "$res/ffmpeg.exe" "$res/ffprobe.exe"

# The import tool and every module it uses (standard library only besides these).
cp "$project/tools/ingest.py" "$project/tools/insv_trailer.py" "$project/tools/insv_motion.py" "$project/tools/clip_tag.py" "$stage/tools/"
python3 -I -c "import sys; sys.path.insert(0, '$stage/tools'); import ingest" || { echo "the packaged import tool does not load" >&2; exit 1; }

# The Scripts-menu entry finds the import tool under %ProgramData% (expanded by the script).
sed -e 's|@INGEST@|%ProgramData%\\Insta360Stitch\\tools\\ingest.py|' "$project/resolve/Insta360 Import.lua" > "$stage/Insta360 Import.lua"

# Text files for Windows: CRLF line ends.
crlf() { sed -e 's/\r*$/\r/' "$1" > "$2"; }
crlf "$project/LICENSE" "$stage/LICENSE.txt"
cat "$project/THIRD_PARTY_NOTICES.txt" "$here/THIRD_PARTY_NOTICES-windows.txt" > "$stage/notices.tmp"
crlf "$stage/notices.tmp" "$stage/THIRD_PARTY_NOTICES.txt"
rm "$stage/notices.tmp"
crlf "$here/README-Windows.txt" "$stage/README-Windows.txt"

makensis -V2 -DVERSION="$version" -DSTAGE="$stage" -DOUTFILE="$project/dist/Insta360Stitch-$version-windows-setup.exe" "$here/Insta360Stitch.nsi"
echo "built: $project/dist/Insta360Stitch-$version-windows-setup.exe"
