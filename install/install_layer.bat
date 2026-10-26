@echo off
rem Enregistre la couche OverlayVR comme couche API OpenXR implicite (tous les jeux OpenXR).
rem A lancer en administrateur, depuis le dossier qui contient la DLL et le .json.
net session >nul 2>&1
if errorlevel 1 (
  echo Lance ce script en tant qu'administrateur ^(clic droit - Executer en tant qu'administrateur^).
  pause
  exit /b 1
)
set "MANIFEST=%~dp0XR_APILAYER_OVERLAYVR_zones.json"
if not exist "%MANIFEST%" (
  echo Fichier introuvable : %MANIFEST%
  pause
  exit /b 1
)
reg add "HKLM\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit" /v "%MANIFEST%" /t REG_DWORD /d 0 /f
echo Couche installee. Redemarre le jeu pour la charger.
pause
