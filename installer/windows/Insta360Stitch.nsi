; Windows installer for Insta360 Stitch (NSIS 3). Built by installer/windows/build_installer.sh.
;
; It installs, for all users:
;   %CommonProgramFiles%\OFX\Plugins\Insta360Stitch.ofx.bundle      the plugin and its helper programs
;   %ProgramData%\Insta360Stitch\tools\                              the import tool (Python, standard library only)
;   %ProgramData%\Blackmagic Design\DaVinci Resolve\Fusion\Scripts\Utility\Insta360 Import.lua
;                                                                    the Workspace > Scripts entry
; and an uninstaller, listed under Settings > Apps.

Unicode true
!include "MUI2.nsh"
!include "x64.nsh"

!ifndef VERSION
  !define VERSION "0.0.0"
!endif
!ifndef STAGE
  !define STAGE "stage"
!endif
!ifndef OUTFILE
  !define OUTFILE "Insta360Stitch-${VERSION}-windows-setup.exe"
!endif

!define APPNAME "Insta360 Stitch for DaVinci Resolve"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Insta360Stitch"
!define BUNDLE "$COMMONFILES64\OFX\Plugins\Insta360Stitch.ofx.bundle"
; With SetShellVarContext all, $APPDATA is %ProgramData%.
!define SUPPORT "$APPDATA\Insta360Stitch"
!define SCRIPTS "$APPDATA\Blackmagic Design\DaVinci Resolve\Fusion\Scripts\Utility"

Name "${APPNAME}"
OutFile "${OUTFILE}"
RequestExecutionLevel admin
ShowInstDetails show
ShowUninstDetails show
SetCompressor /SOLID lzma
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APPNAME}"
VIAddVersionKey "FileDescription" "${APPNAME} installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "GPL-3.0; see LICENSE"

!define MUI_ICON "${NSISDIR}\Contrib\Graphics\Icons\modern-install.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\modern-uninstall.ico"
!define MUI_WELCOMEPAGE_TITLE "Insta360 Stitch ${VERSION} for Windows"
!define MUI_WELCOMEPAGE_TEXT "Stitches and reframes Insta360 X5 360 footage in DaVinci Resolve Studio 21.1 or later, without exporting from Insta360 Studio first.$\r$\n$\r$\nQuit DaVinci Resolve before you continue.$\r$\n$\r$\nWindows port of the free, open source macOS plugin by Ilias823 (github.com/Ilias823/insta360-resolve)."
!define MUI_FINISHPAGE_TITLE "Insta360 Stitch is installed"
!define MUI_FINISHPAGE_TEXT "Open DaVinci Resolve and use Workspace > Scripts > Insta360 Import to bring in .insv clips. Put a '<name> 360' clip on a timeline and add Effects > Open FX > Insta360 > Insta360 Stitch."

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${STAGE}\LICENSE.txt"
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "Insta360 Stitch needs 64-bit Windows, like DaVinci Resolve."
    Abort
  ${EndIf}
  SetRegView 64
  SetShellVarContext all
FunctionEnd

Function un.onInit
  SetRegView 64
  SetShellVarContext all
FunctionEnd

Section "Insta360 Stitch" SecMain
  SectionIn RO
  SetShellVarContext all

  ; The plugin bundle; an older version is replaced as a whole.
  RMDir /r "${BUNDLE}"
  SetOutPath "${BUNDLE}\Contents"
  File /r "${STAGE}\Insta360Stitch.ofx.bundle\Contents\*.*"

  ; The import tool and the modules it uses.
  SetOutPath "${SUPPORT}\tools"
  File "${STAGE}\tools\ingest.py"
  File "${STAGE}\tools\insv_trailer.py"
  File "${STAGE}\tools\insv_motion.py"
  File "${STAGE}\tools\clip_tag.py"
  SetOutPath "${SUPPORT}"
  File "${STAGE}\LICENSE.txt"
  File "${STAGE}\THIRD_PARTY_NOTICES.txt"
  File "${STAGE}\README-Windows.txt"

  ; The Workspace > Scripts entry for all users. A per-user copy from a development setup
  ; would show the entry twice, so it goes.
  SetOutPath "${SCRIPTS}"
  File "${STAGE}\Insta360 Import.lua"
  SetShellVarContext current
  Delete "$APPDATA\Blackmagic Design\DaVinci Resolve\Support\Fusion\Scripts\Utility\Insta360 Import.lua"
  SetShellVarContext all

  WriteUninstaller "${SUPPORT}\Uninstall.exe"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayName" "${APPNAME}"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINSTKEY}" "Publisher" "insta360-resolve (open source)"
  WriteRegStr HKLM "${UNINSTKEY}" "URLInfoAbout" "https://github.com/Ilias823/insta360-resolve"
  WriteRegStr HKLM "${UNINSTKEY}" "InstallLocation" "${SUPPORT}"
  WriteRegStr HKLM "${UNINSTKEY}" "UninstallString" '"${SUPPORT}\Uninstall.exe"'
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  SetShellVarContext all
  RMDir /r "${BUNDLE}"
  Delete "${SCRIPTS}\Insta360 Import.lua"
  RMDir /r "${SUPPORT}\tools"
  Delete "${SUPPORT}\LICENSE.txt"
  Delete "${SUPPORT}\THIRD_PARTY_NOTICES.txt"
  Delete "${SUPPORT}\README-Windows.txt"
  Delete "${SUPPORT}\Uninstall.exe"
  RMDir "${SUPPORT}"
  DeleteRegKey HKLM "${UNINSTKEY}"
  ; The clips' lens files, the registry of imported clips (%APPDATA%\Insta360Stitch) and the logs
  ; are the user's and stay.
SectionEnd
