# Insta360 Stitch for DaVinci Resolve

Edit **Insta360 X5** 360° footage directly in **DaVinci Resolve Studio** on the Mac: stitch it, reframe it into normal or vertical video, and follow subjects automatically, without exporting from Insta360 Studio first.

> **Windows port.** This copy also builds for Windows (x64): see [Windows](#windows) below. The macOS build files are unchanged but the shared sources now target the CPU/FFmpeg code paths, so build macOS from the original 0.4.1 release.

Free and open source. If it saves you time, you can support its development on **[Ko-fi](https://ko-fi.com/ilias823)**.

## Features

- **Import**: Workspace > Scripts > Insta360 Import turns `.insv` clips into ready-to-edit `<name> 360` clips in an *Insta360* bin (no re-encoding; the lens calibration is read from each file and refined automatically). The lens files they are made from are kept out of the way in *Insta360 > Source files*.
- **Stitch**: dual-fisheye to equirectangular on the GPU (Metal), with per-frame lens matching and an automatic stitch distance for close subjects.
- **Reframe**: pan, tilt, roll, field of view and distance, with Insta360 Studio-style looks (Tiny Planet, Mega View, Ultra Wide, Dewarp, Linear). Works in timelines of any shape, including vertical. **Remove Distortion** keeps straight lines straight, like Insta360's linear view.
- **Full resolution**: reads the lenses straight from the clip's own files (Apple's hardware decoder), so reframed views are as sharp as Insta360 Studio's in any timeline size, even a vertical 1080x1920 one where Resolve passes on only 540 pixels per lens. Falls back to Resolve's picture when the file is missing or does not match the frame (for example inside a Fusion comp). Can be turned off in the controls.
- **Stabilization**: from the camera's own motion sensor, like Insta360's FlowState: Smooth (follows your camera moves), Locked Direction, and Level Horizon. Needs clips imported with version 0.2 or later.
- **Hyperlapse**: stabilization follows each frame's real capture time, and smooths more on sped-up clips. Motion Blur adds long-exposure streaks (try 50); it adds time per frame, so turn it on when the edit is done. Re-import clips made with earlier versions.
- **On-screen controls**: drag the picture to aim, drag the zoom bar to zoom (viewer set to *Open FX Overlay*).
- **Subject tracking**: put the box on a subject and press Track Subject; the view follows it as Pan/Tilt keyframes.
- **Updates**: About > Check for Updates looks for new releases here.

## Requirements

- Mac with Apple Silicon (M1 or newer), macOS 13 or later
- DaVinci Resolve Studio 21.1 or later
- Insta360 X5 footage (other models are untested)

## Install

1. Download the latest `Insta360Stitch-<version>.pkg` from [Releases](https://github.com/Ilias823/insta360-resolve/releases).
2. Quit DaVinci Resolve and double-click the installer.
3. The installer is not signed with an Apple developer certificate yet, so macOS blocks it the first time: open **System Settings > Privacy & Security** and click **Open Anyway** (on macOS 14 and earlier you can also right-click the file and choose Open).
4. Open Resolve and use **Workspace > Scripts > Insta360 Import** to bring in clips. Put a clip on a timeline and add **Effects > Open FX > Insta360 > Insta360 Stitch**.

To remove it: `sudo "/Library/Application Support/Insta360Stitch/uninstall.sh"`.

## Build from source

Needs Xcode and DaVinci Resolve (for its OpenFX SDK).

```sh
make -C plugin            # build plugin/build/Insta360Stitch.ofx.bundle
make -C plugin test       # checks that need no host application
make -C plugin install    # copy to /Library/OFX/Plugins (create it, writable, first)
tools/install_resolve_menu.sh   # Scripts menu entry using this folder's import tool
installer/build_pkg.sh    # dist/Insta360Stitch-<version>.pkg
```

`tools/` also holds the Python reference implementation (`stitch_poc.py`) and the comparison test (`compare_metal.py`) that keeps the Metal stitch in step with it; those need `numpy` and `opencv-python-headless`.

## Releasing

1. Raise `CFBundleShortVersionString` in `plugin/Info.plist`.
2. Commit and push.
3. `installer/release.sh "What is new"` builds the installer and publishes the GitHub release that Check for Updates finds.

## How it works

The X5 records the two lenses as two HEVC streams in one `.insv` file, with the factory calibration in a trailer at the end. The import copies each lens into its own `.mov` (Resolve reads one video stream per file) and builds a compound clip with lens A on the left half and lens B on the right half, a layout that holds in any timeline shape. The OpenFX plugin then stitches or reframes that compound clip using the calibration. Resolve gives an effect only as many pixels as the timeline has, so with Full Resolution on the plugin decodes the lens files itself, keeps them as mipmapped Metal textures, and checks on every frame on the GPU that the decoded frame is the one Resolve passed in.

## Credits and licence

Licensed under the [GNU General Public License v3.0](LICENSE).

- The X5 calibration layout, lens model and seam refinement follow [insv-stitch](https://github.com/BenjaminHenriksson/insv-stitch) by Benjamin Henriksson (MIT).
- The plugin is built on the OpenFX support library (BSD) from the OpenFX SDK.

See [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt). Insta360 and DaVinci Resolve are trademarks of their owners; this project is not affiliated with them.

## Windows

The Windows build renders on the CPU (all cores) instead of Metal and decodes the lens files with a static FFmpeg (on the GPU's video decoder through Direct3D 11 where the driver offers it), so it needs no extra DLLs. Everything else works as on the Mac: import, full resolution, stabilization, motion blur, tracking, on-screen controls, updates.

| macOS original | Windows port |
|---|---|
| `MetalKernel.mm` (Metal shaders) | `CpuKernel.cpp`, the same maths line by line, multithreaded; mip levels and anisotropic filtering in software |
| `LensDecoder.mm` (AVFoundation) | `LensDecoder.cpp` (FFmpeg, D3D11VA/DXVA2 with software fallback) |
| `MotionBlur.mm` (Vision optical flow) | `MotionBlur.cpp` (pyramidal Lucas-Kanade with confidence-weighted smoothing) |
| `ProgressHelper.mm` (Cocoa) | `ProgressHelper_win.cpp` → `TrackProgress.exe` (Win32) |
| `Updater.mm` (NSURLSession) | `Updater.cpp` (WinHTTP) |
| `MediaTool.mm` | `ffmpeg.exe` / `ffprobe.exe` in the bundle (ingest.py's existing ffmpeg path, plus the ProRes 4444 tag movie) |
| `~/Library/...` | `%APPDATA%\Insta360Stitch` (clip registry, logs), `%ProgramData%\Insta360Stitch` (import tool) |
| osascript dialogs | PowerShell dialogs; clip lists go through UTF-8 files so paths with umlauts work |

### Requirements

- 64-bit Windows 10 or 11
- DaVinci Resolve Studio 21.1 or later (its bundled `ResolvePython.exe` runs the import; with older versions install Python 3 from python.org)

### Build (Linux or MSYS2, MinGW-w64 and NSIS)

```sh
tools/build_ffmpeg_windows.sh /path/to/ffmpeg-7.1-source deps/ffwin   # static LGPL FFmpeg, once
make -C plugin -f Makefile.win FFMPEG=$PWD/deps/ffwin                  # plugin/build-win/Insta360Stitch.ofx.bundle
FFMPEG=$PWD/deps/ffwin installer/windows/build_installer.sh           # dist/Insta360Stitch-<version>-windows-setup.exe
```

The OpenFX SDK is vendored in `plugin/third_party` (Academy Software Foundation, BSD), with the draw suite added to the support library for the on-screen controls (Resolve's own SDK copy has it; the public one does not).

### Tests

`make -C plugin -f Makefile.win PLATFORM=native test` builds the same code for Linux against the system FFmpeg and runs the self-checks and the optical-flow checks. `tools/compare_kernel.py` compares the CPU kernel with the Python reference (`stitch_poc.py`), as `compare_metal.py` did for Metal; `test_plugin --fullres lensA.mov lensB.mov ...` checks full-resolution decoding and the frame check against an independent decode.

### Install

Run `Insta360Stitch-<version>-windows-setup.exe` with DaVinci Resolve closed. It is not code-signed, so Windows SmartScreen asks first (More info > Run anyway). Uninstall under Settings > Apps.
