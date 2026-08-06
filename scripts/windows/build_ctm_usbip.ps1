# Build CTM-USBIP and copy Release/Debug output to dist/CTM-USBIP/
#
# CTM-USBIP is the HOST-side agent. It is not part of the TV package and is no
# longer carried as a submodule here — it was pinned to a commit that exists on
# no remote, which broke every recursive clone. Check it out yourself and point
# -CtmRoot at it (or drop it next to this repo as ../CTM-USBIP):
#
#     git clone https://github.com/CTM-Bridge/CTM-USBIP.git
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$CtmRoot
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $CtmRoot) {
    $CtmRoot = Join-Path (Split-Path -Parent $ProjectRoot) 'CTM-USBIP'
}

if (-not (Test-Path (Join-Path $CtmRoot 'build.ps1'))) {
    Write-Error "No CTM-USBIP checkout at $CtmRoot. Clone https://github.com/CTM-Bridge/CTM-USBIP.git and pass -CtmRoot <path>."
}

Push-Location $CtmRoot
try {
    & .\build.ps1 -Configuration $Configuration
} finally {
    Pop-Location
}

$out = Join-Path $CtmRoot "out\x64\$Configuration"
$dist = Join-Path $ProjectRoot 'dist\CTM-USBIP'
New-Item -ItemType Directory -Force -Path $dist | Out-Null
Copy-Item -Force -Path (Join-Path $out '*') -Destination $dist -Recurse
Write-Host "Copied CTM-USBIP build to $dist"
