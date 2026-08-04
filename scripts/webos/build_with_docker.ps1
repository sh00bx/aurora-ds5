# Build Aurora for LG webOS via Docker
# Works on Windows without WSL Ubuntu - uses an Ubuntu container

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

Write-Host "=== Aurora - Build for LG webOS (via Docker) ===" -ForegroundColor Cyan
Write-Host ""

# Check Docker
$dockerOk = $false
$ErrorActionPreferenceBak = $ErrorActionPreference
$ErrorActionPreference = "SilentlyContinue"
& docker ps *>$null
$dockerOk = ($LASTEXITCODE -eq 0)
$ErrorActionPreference = $ErrorActionPreferenceBak
if (-not $dockerOk) {
    Write-Host "ERROR: Docker is not running or is not installed." -ForegroundColor Red
    Write-Host "  - Install Docker Desktop: https://www.docker.com/products/docker-desktop" -ForegroundColor Yellow
    Write-Host "  - Or use WSL with Ubuntu: wsl --install -d Ubuntu" -ForegroundColor Yellow
    Write-Host "  - Then run: ./scripts/webos/build_for_lg.sh (inside WSL)" -ForegroundColor Yellow
    exit 1
}

Write-Host "Using Docker to build in Ubuntu environment..." -ForegroundColor Green
Write-Host ""

# Mount the project and run the build
$ScriptPath = Join-Path $PSScriptRoot "docker_build_inner.sh"
. (Join-Path $PSScriptRoot "docker_build_invoke.ps1")
$exitCode = Invoke-AuroraWebOSDockerBuild -ProjectRoot $ProjectRoot -InnerScriptPath $ScriptPath -Env @{
    CI                    = "1"
    DOCKER_SKIP_SUBMODULES = "1"
}

if ($exitCode -eq 0) {
    Write-Host ""
    Write-Host "=== Build complete! ===" -ForegroundColor Green
    Write-Host "Package in: $ProjectRoot\dist\" -ForegroundColor Cyan
    Get-ChildItem "$ProjectRoot\dist\*.ipk" -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  $($_.Name)" }
} else {
    Write-Host ""
    Write-Host "Build failed. Scroll up for the Docker log (apt/DNS, cmake, etc.)." -ForegroundColor Red
    exit 1
}
