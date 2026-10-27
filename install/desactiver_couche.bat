@echo off
rem Desactive la couche OverlayVR dans OpenXR : plus aucun jeu ne la charge (effet au prochain lancement du jeu).
net session >nul 2>&1 || (powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs" & exit /b)
reg add "HKLM\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit" /v "%~dp0XR_APILAYER_OVERLAYVR_zones.json" /t REG_DWORD /d 1 /f
echo.
echo Couche OverlayVR DESACTIVEE. Relance le jeu pour que ce soit pris en compte.
pause
