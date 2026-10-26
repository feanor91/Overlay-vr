@echo off
net session >nul 2>&1
if errorlevel 1 (
  echo Lance ce script en tant qu'administrateur.
  pause
  exit /b 1
)
set "MANIFEST=%~dp0XR_APILAYER_OVERLAYVR_zones.json"
reg delete "HKLM\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit" /v "%MANIFEST%" /f
echo Couche desinstallee.
pause
