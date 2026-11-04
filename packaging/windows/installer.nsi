Unicode true
Name "Relay"
OutFile "${OUTPUT}"
InstallDir "$LOCALAPPDATA\Programs\Relay"
RequestExecutionLevel user
SetCompressor /SOLID lzma
!include "MUI2.nsh"
!define MUI_ICON "../../assets/relay.ico"
!define MUI_UNICON "../../assets/relay.ico"
!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\Relay.exe"
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"
Section "Relay"
 SetOutPath "$INSTDIR"
 File /r "${STAGE}/*"
 WriteUninstaller "$INSTDIR\Uninstall.exe"
 CreateShortcut "$SMPROGRAMS\Relay.lnk" "$INSTDIR\Relay.exe"
 WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Relay" "DisplayName" "Relay"
 WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Relay" "DisplayIcon" "$INSTDIR\Relay.exe,0"
 WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Relay" "DisplayVersion" "${RELEASE_ID}"
 WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Relay" "UninstallString" '$\"$INSTDIR\Uninstall.exe$\"'
 WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Relay" "InstallLocation" "$INSTDIR"
SectionEnd
Section "Uninstall"
 Delete "$SMPROGRAMS\Relay.lnk"
 DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Relay"
 ; Remove only Relay program files. Accounts and downloaded update versions remain in AppData.
 RMDir /r "$INSTDIR\bin"
 RMDir /r "$INSTDIR\lib"
 RMDir /r "$INSTDIR\plugins"
 RMDir /r "$INSTDIR\qml"
 RMDir /r "$INSTDIR\translations"
 RMDir /r "$INSTDIR\share"
 Delete "$INSTDIR\Relay.exe"
 Delete "$INSTDIR\README.md"
 Delete "$INSTDIR\build-info.json"
 Delete "$INSTDIR\build-validation.json"
 Delete "$INSTDIR\runtime-dependencies.json"
 Delete "$INSTDIR\SHA256.json"
 Delete "$INSTDIR\Uninstall.exe"
 RMDir "$INSTDIR"
SectionEnd
