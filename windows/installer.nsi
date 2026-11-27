; NSIS installer for scrcpy Windows custom builds.
;
; Design goals:
;   - no registry reads or writes
;   - no PATH manipulation or other environment-variable dependencies
;   - install vs upgrade is decided by a plain version.txt in the install dir
;   - the real executable and every DLL live in the hard-coded dll\ subdir
;   - user-editable config and templates are preserved on upgrade
;
; Build from CI:
;   makensis //DSRC_DIR=<path-to-dist> //DAPP_VERSION=<version> windows/installer.nsi

!include LogicLib.nsh

!define APP_NAME "scrcpy Windows Custom Build"
!define APP_VERSION "DEV"
!define SRC_DIR "."

Name "${APP_NAME} ${APP_VERSION}"
OutFile "scrcpy-setup.exe"
InstallDir "$LOCALAPPDATA\Programs\scrcpy"
RequestExecutionLevel user
SetCompressor /SOLID lzma
ShowInstDetails show

Var UpgradeFrom

Section "Install"
  ; A version.txt in the chosen install directory means this is an upgrade.
  IfFileExists "$INSTDIR\version.txt" read_version fresh_install

read_version:
  FileOpen $0 "$INSTDIR\version.txt" r
  FileRead $0 $UpgradeFrom
  FileClose $0
  DetailPrint "Upgrading from $UpgradeFrom; config and templates will be kept."
  RMDir /r "$INSTDIR\dll"
  Goto install_files

fresh_install:
  DetailPrint "No previous installation found; performing a fresh install."

install_files:
  ; All DLLs and the real executable live together in dll\.
  CreateDirectory "$INSTDIR\dll"
  SetOutPath "$INSTDIR\dll"
  File /r "${SRC_DIR}\dll\*"

  SetOutPath "$INSTDIR"
  File "${SRC_DIR}\scrcpy.exe"
  File "${SRC_DIR}\scrcpy-server"
  File "${SRC_DIR}\version.txt"

  ; User-editable mapping files are only written when missing.
  IfFileExists "$INSTDIR\scrcpy_mapping_config.json" keep_config
  File "${SRC_DIR}\scrcpy_mapping_config.json"
keep_config:

  IfFileExists "$INSTDIR\templates" keep_templates
  File /r "${SRC_DIR}\templates"
keep_templates:

  WriteUninstaller "$INSTDIR\uninstall.exe"

  CreateDirectory "$SMPROGRAMS\scrcpy"
  CreateShortcut "$SMPROGRAMS\scrcpy\scrcpy.lnk" "$INSTDIR\scrcpy.exe"
  CreateShortcut "$SMPROGRAMS\scrcpy\Uninstall.lnk" "$INSTDIR\uninstall.exe"
SectionEnd

Section "Uninstall"
  Delete "$INSTDIR\scrcpy.exe"
  Delete "$INSTDIR\scrcpy-server"
  Delete "$INSTDIR\version.txt"
  Delete "$INSTDIR\scrcpy_mapping_config.json"
  Delete "$INSTDIR\uninstall.exe"
  RMDir /r "$INSTDIR\dll"
  RMDir /r "$INSTDIR\templates"

  Delete "$SMPROGRAMS\scrcpy\scrcpy.lnk"
  Delete "$SMPROGRAMS\scrcpy\Uninstall.lnk"
  RMDir "$SMPROGRAMS\scrcpy"

  RMDir /r "$INSTDIR"
SectionEnd
