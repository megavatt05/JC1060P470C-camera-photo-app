# Fix esp_ipa 1.1.x for ESP-IDF 6.0: replace hal/isp_types.h with driver/isp_types.h
# Run AFTER: idf.py set-target / reconfigure (when managed_components exists)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$hdr = Join-Path $root "managed_components\espressif__esp_ipa\include\esp_ipa_types.h"
if (-not (Test-Path $hdr)) {
  Write-Host "managed_components not found. Run first: idf.py reconfigure"
  Write-Host "Expected: $hdr"
  exit 1
}
$c = Get-Content $hdr -Raw
if ($c -match 'driver/isp_types\.h') {
  Write-Host "Already patched."
  exit 0
}
$c2 = $c -replace '#include\s+"hal/isp_types\.h"', '#include "driver/isp_types.h"'
if ($c2 -eq $c) {
  Write-Host "Pattern not found in $hdr"
  exit 1
}
Set-Content -Path $hdr -Value $c2 -NoNewline
Write-Host "OK: patched $hdr"
Write-Host "  #include \"hal/isp_types.h\" -> #include \"driver/isp_types.h\""
