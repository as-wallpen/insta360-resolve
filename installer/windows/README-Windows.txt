Insta360 Stitch for DaVinci Resolve - Windows
=============================================

Stitches and reframes Insta360 X5 360 footage inside DaVinci Resolve Studio,
without exporting from Insta360 Studio first. Windows port of the free, open
source macOS plugin by Ilias823: https://github.com/Ilias823/insta360-resolve

Needs: 64-bit Windows 10 or 11, DaVinci Resolve Studio 21.1 or later (which
brings its own Python for the import; with older versions install Python 3
from python.org).

Use
---
1. Quit DaVinci Resolve, run the installer, open Resolve.
2. Workspace > Scripts > Insta360 Import: choose .insv clips. The lens files
   and calibration go into an "insta360_ingest" folder next to each clip.
3. Put a "<name> 360" clip from the "Insta360" bin on a timeline and add
   Effects > Open FX > Insta360 > Insta360 Stitch.

Differences from the macOS version
----------------------------------
- The effect renders on the CPU (all cores) instead of the GPU. Turn off
  Full Resolution in the effect if playback is too slow; use Resolve's render
  cache for smooth playback.
- The lens files are decoded with FFmpeg, on the graphics card's video decoder
  (Direct3D 11) where available, otherwise in software.
- Motion Blur measures movement with its own optical flow (Lucas-Kanade)
  instead of Apple's Vision framework.

Files
-----
Plugin:        %CommonProgramFiles%\OFX\Plugins\Insta360Stitch.ofx.bundle
Import tool:   %ProgramData%\Insta360Stitch\tools
Scripts menu:  %ProgramData%\Blackmagic Design\DaVinci Resolve\Fusion\Scripts\Utility
Clip registry: %APPDATA%\Insta360Stitch (kept when uninstalling)
Logs:          %APPDATA%\Insta360Stitch\Logs\Insta360Stitch.log,
               %APPDATA%\Insta360Stitch\Insta360Import.log

Uninstall under Settings > Apps > Insta360 Stitch for DaVinci Resolve.
