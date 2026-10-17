; NSIS installer for 5ch Viewer (Windows x64)
;   makensis -DVERSION=1.4.0 -DEXE=5chviewer.exe -DOUTFILE=dist/5chviewer-1.4.0-setup.exe packaging/5chviewer.nsi
Unicode true
!ifndef VERSION
  !define VERSION "1.4.0"
!endif
!ifndef EXE
  !define EXE "5chviewer.exe"
!endif
!ifndef OUTFILE
  !define OUTFILE "5chviewer-${VERSION}-setup.exe"
!endif

!define APPNAME "5ch Viewer"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\5chViewer"

Name "${APPNAME} ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\${APPNAME}"
InstallDirRegKey HKLM "Software\5chViewer" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
BrandingText "${APPNAME} ${VERSION}"

!include "MUI2.nsh"
!include "x64.nsh"

!define MUI_ICON "5chviewer.ico"
!define MUI_UNICON "5chviewer.ico"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\5chviewer.exe"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "Japanese"
!insertmacro MUI_LANGUAGE "English"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APPNAME}"
VIAddVersionKey "FileDescription" "${APPNAME} installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "Copyright (c) 2026 5ch Viewer authors (MIT License)"

LangString SecMain ${LANG_JAPANESE} "5ch Viewer (必須)"
LangString SecMain ${LANG_ENGLISH}  "5ch Viewer (required)"
LangString SecStart ${LANG_JAPANESE} "スタートメニューにショートカットを作成"
LangString SecStart ${LANG_ENGLISH}  "Start Menu shortcut"
LangString SecDesk ${LANG_JAPANESE} "デスクトップにショートカットを作成"
LangString SecDesk ${LANG_ENGLISH}  "Desktop shortcut"

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "64-bit Windows is required. / 64ビット版 Windows が必要です。"
    Abort
  ${EndIf}
  SetRegView 64
FunctionEnd

Function un.onInit
  SetRegView 64
FunctionEnd

Section "$(SecMain)" SEC_MAIN
  SectionIn RO
  SetOutPath "$INSTDIR"
  File "${EXE}"
  File /oname=README.md "..\README.md"
  File "THIRD_PARTY_NOTICES.txt"
  File /oname=LICENSE.txt "..\LICENSE"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "Software\5chViewer" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayName" "${APPNAME}"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayIcon" "$INSTDIR\5chviewer.exe"
  WriteRegStr HKLM "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTKEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoRepair" 1
SectionEnd

Section "$(SecStart)" SEC_START
  CreateDirectory "$SMPROGRAMS\${APPNAME}"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk" "$INSTDIR\5chviewer.exe"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\Uninstall.lnk" "$INSTDIR\uninstall.exe"
SectionEnd

Section /o "$(SecDesk)" SEC_DESK
  CreateShortcut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\5chviewer.exe"
SectionEnd

Section "Uninstall"
  Delete "$INSTDIR\5chviewer.exe"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\THIRD_PARTY_NOTICES.txt"
  Delete "$INSTDIR\LICENSE.txt"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk"
  Delete "$SMPROGRAMS\${APPNAME}\Uninstall.lnk"
  RMDir "$SMPROGRAMS\${APPNAME}"
  Delete "$DESKTOP\${APPNAME}.lnk"
  DeleteRegKey HKLM "${UNINSTKEY}"
  DeleteRegKey HKLM "Software\5chViewer"
  ; settings / read-state / dat cache live in %APPDATA%\5chviewer and are kept on purpose
SectionEnd
