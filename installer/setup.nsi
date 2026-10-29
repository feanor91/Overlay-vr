; Installateur OverlayVR : copie les fichiers et enregistre la couche OpenXR (HKLM, 64 bits).
; Usage : makensis /DVERSION=0.0.1 installer/setup.nsi   (le dossier ..\dist doit contenir les binaires)
!include "MUI2.nsh"

!ifndef VERSION
  !define VERSION "dev"
!endif

Name "OverlayVR ${VERSION}"
OutFile "..\OverlayVR-Setup-${VERSION}.exe"
InstallDir "$PROGRAMFILES64\OverlayVR"
InstallDirRegKey HKLM "Software\OverlayVR" "InstallDir"
RequestExecutionLevel admin
Unicode true

!define LAYER_KEY "SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit"
!define LAYER_JSON "$INSTDIR\XR_APILAYER_OVERLAYVR_zones.json"

Function LaunchTray
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\OverlayVR_Tray.exe"'
FunctionEnd
!define MUI_FINISHPAGE_RUN ""
!define MUI_FINISHPAGE_RUN_TEXT "Lancer OverlayVR (icone dans la barre d'etat)"
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchTray
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "French"

Section "Install"
  SetRegView 64
  nsExec::Exec 'taskkill /F /IM OverlayVR_Tray.exe'
  nsExec::Exec 'taskkill /F /IM OverlayVR_Config.exe'
  nsExec::Exec 'taskkill /F /IM OverlayVR_Editor.exe'
  SetOutPath "$INSTDIR"
  ; La couche est chargee par tout programme OpenXR ouvert (MSFS, editeur...) : Windows interdit alors de l'ecraser mais
  ; permet de la renommer. On l'ecarte ; elle est supprimee des que possible (au redemarrage au plus tard).
  IfFileExists "$INSTDIR\XR_APILAYER_OVERLAYVR_zones.dll" 0 noOldLayer
    Delete "$INSTDIR\XR_APILAYER_OVERLAYVR_zones.old"
    Rename "$INSTDIR\XR_APILAYER_OVERLAYVR_zones.dll" "$INSTDIR\XR_APILAYER_OVERLAYVR_zones.old"
    Delete /REBOOTOK "$INSTDIR\XR_APILAYER_OVERLAYVR_zones.old"
  noOldLayer:
  File "..\dist\XR_APILAYER_OVERLAYVR_zones.dll"
  File "..\dist\XR_APILAYER_OVERLAYVR_zones.json"
  File "..\dist\OverlayVR_Editor.exe"
  File "..\dist\README.md"
  File "..\dist\OverlayVR_Tray.exe"
  File "..\dist\OverlayVR_Config.exe"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  ; Couche API OpenXR implicite : valeur 0 = activee.
  WriteRegDWORD HKLM "${LAYER_KEY}" "${LAYER_JSON}" 0
  WriteRegStr HKLM "Software\OverlayVR" "InstallDir" "$INSTDIR"

  ; Reglages utilisateur : cree le fichier par defaut s'il n'existe pas (jamais ecrase).
  SetShellVarContext current
  CreateDirectory "$APPDATA\OverlayVR"
  IfFileExists "$APPDATA\OverlayVR\settings.json" settings_done 0
    FileOpen $0 "$APPDATA\OverlayVR\settings.json" w
    FileWrite $0 '{$\r$\n  "enabled": true,$\r$\n  "mode": "allowlist",$\r$\n  "apps": ["FlightSimulator2024.exe", "FlightSimulator.exe"]$\r$\n}$\r$\n'
    FileClose $0
  settings_done:

  CreateDirectory "$SMPROGRAMS\OverlayVR"
  CreateShortcut "$SMPROGRAMS\OverlayVR\OverlayVR (barre d'etat).lnk" "$INSTDIR\OverlayVR_Tray.exe"
  CreateShortcut "$SMPROGRAMS\OverlayVR\OverlayVR Editeur.lnk" "$INSTDIR\OverlayVR_Editor.exe"
  CreateShortcut "$SMPROGRAMS\OverlayVR\OverlayVR Configuration.lnk" "$INSTDIR\OverlayVR_Config.exe"
  CreateShortcut "$SMPROGRAMS\OverlayVR\Desinstaller OverlayVR.lnk" "$INSTDIR\Uninstall.exe"

  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OverlayVR" "DisplayName" "OverlayVR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OverlayVR" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OverlayVR" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OverlayVR" "NoModify" 1
SectionEnd

Section "Uninstall"
  SetRegView 64
  nsExec::Exec 'taskkill /F /IM OverlayVR_Tray.exe'
  nsExec::Exec 'taskkill /F /IM OverlayVR_Config.exe'
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "OverlayVR"
  DeleteRegValue HKLM "${LAYER_KEY}" "${LAYER_JSON}"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OverlayVR"
  DeleteRegKey HKLM "Software\OverlayVR"
  Delete "$SMPROGRAMS\OverlayVR\*.lnk"
  RMDir "$SMPROGRAMS\OverlayVR"
  Delete "$INSTDIR\XR_APILAYER_OVERLAYVR_zones.dll"
  Delete "$INSTDIR\XR_APILAYER_OVERLAYVR_zones.json"
  Delete "$INSTDIR\OverlayVR_Editor.exe"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\OverlayVR_Tray.exe"
  Delete "$INSTDIR\OverlayVR_Config.exe"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
SectionEnd
