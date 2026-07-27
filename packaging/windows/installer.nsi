; packaging/windows/installer.nsi
; NSIS installer script for GameModManager on Windows
;
; Build with: makensis installer.nsi
; Expects gamemodmanager/ directory with the built exe + plugins + Qt DLLs

!include "MUI2.nsh"
!include "FileFunc.nsh"

; ── Version (read from CMakeLists.txt or passed via /D flag) ──
!ifndef VERSION
    !define VERSION "0.1.0"
!endif

Name "GameModManager ${VERSION}"
OutFile "GameModManager-${VERSION}-Setup.exe"
InstallDir "$PROGRAMFILES\GameModManager"
InstallDirRegKey HKCU "Software\GameModManager" "InstallDir"
RequestExecutionLevel admin

; ── Pages ──
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\..\docs\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; ── Installer ──
Section "GameModManager" SecMain
    SetOutPath "$INSTDIR"

    ; Main executable
    File "gamemodmanager.exe"

    ; Plugins
    SetOutPath "$INSTDIR\plugins"
    File /r "plugins\*.*"

    ; Qt and runtime DLLs (placed next to the exe)
    SetOutPath "$INSTDIR"
    File /nonfatal "*.dll"

    ; Create instance directories
    CreateDirectory "$INSTDIR\config"
    CreateDirectory "$INSTDIR\mods"
    CreateDirectory "$INSTDIR\downloads"
    CreateDirectory "$INSTDIR\cache"
    CreateDirectory "$INSTDIR\logs"

    ; Write instance.toml template
    FileOpen $0 "$INSTDIR\instance.toml" w
    FileWrite $0 "# GameModManager instance$\r$\n"
    FileWrite $0 "game_id = &quot;&quot;$\r$\n"
    FileWrite $0 "game_name = &quot;&quot;$\r$\n"
    FileClose $0

    ; Store install path in registry
    WriteRegStr HKCU "Software\GameModManager" "InstallDir" "$INSTDIR"

    ; Register uninstaller
    WriteUninstaller "$INSTDIR\Uninstall.exe"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\GameModManager" \
        "DisplayName" "GameModManager"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\GameModManager" \
        "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\GameModManager" \
        "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\GameModManager" \
        "DisplayVersion" "${VERSION}"
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\GameModManager" \
        "NoModify" 1
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\GameModManager" \
        "NoRepair" 1

    ; Register nxm:// protocol handler
    WriteRegStr HKCU "Software\Classes\nxm" "" "URL:NXM Protocol"
    WriteRegStr HKCU "Software\Classes\nxm" "URL Protocol" ""
    WriteRegStr HKCU "Software\Classes\nxm\shell\open\command" "" \
        '"$INSTDIR\gamemodmanager.exe" --handle-nxm "%1"'

    ; Start Menu shortcut
    CreateDirectory "$SMPROGRAMS\GameModManager"
    CreateShortCut "$SMPROGRAMS\GameModManager\GameModManager.lnk" "$INSTDIR\gamemodmanager.exe"
    CreateShortCut "$SMPROGRAMS\GameModManager\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
SectionEnd

; ── Uninstaller ──
Section "Uninstall"
    ; Remove files
    RMDir /r "$INSTDIR\plugins"
    RMDir /r "$INSTDIR\config"
    RMDir /r "$INSTDIR\logs"
    Delete "$INSTDIR\gamemodmanager.exe"
    Delete "$INSTDIR\instance.toml"
    Delete "$INSTDIR\Uninstall.exe"
    Delete "$INSTDIR\*.dll"
    RMDir "$INSTDIR"

    ; Remove registry entries
    DeleteRegKey HKCU "Software\GameModManager"
    DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\GameModManager"
    DeleteRegKey HKCU "Software\Classes\nxm"

    ; Remove Start Menu shortcuts
    RMDir /r "$SMPROGRAMS\GameModManager"
SectionEnd
