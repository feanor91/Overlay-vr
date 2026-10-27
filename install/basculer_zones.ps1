# Active/desactive l'affichage des zones sans droits administrateur, meme jeu lance (champ "enabled" de settings.json).
$p = Join-Path $env:APPDATA 'OverlayVR\settings.json'
New-Item -ItemType Directory -Force -Path (Split-Path $p) | Out-Null
if (Test-Path $p) { $j = [System.IO.File]::ReadAllText($p) | ConvertFrom-Json } else { $j = [pscustomobject]@{ enabled = $true; mode = 'allowlist'; apps = @('FlightSimulator2024.exe', 'FlightSimulator.exe') } }
if ($null -eq $j.enabled) { $j | Add-Member -NotePropertyName enabled -NotePropertyValue $true }
$j.enabled = -not $j.enabled
[System.IO.File]::WriteAllText($p, ($j | ConvertTo-Json -Depth 5), (New-Object System.Text.UTF8Encoding $false))
Add-Type -AssemblyName PresentationFramework
$etat = if ($j.enabled) { 'ACTIVEES' } else { 'DESACTIVEES' }
[void][System.Windows.MessageBox]::Show("Zones OverlayVR $etat (effet immediat, meme jeu lance).", 'OverlayVR')
