# Shared docker run helper for Aurora webOS builds (Windows Docker Desktop).
# Fixes common "Could not resolve archive.ubuntu.com" DNS failures inside containers.

function Invoke-AuroraWebOSDockerBuild {
    param(
        [Parameter(Mandatory = $true)]
        [string] $ProjectRoot,
        [Parameter(Mandatory = $true)]
        [string] $InnerScriptPath,
        [hashtable] $Env = @{}
    )

    $dockerArgs = @(
        "run", "--rm",
        "--dns", "8.8.8.8",
        "--dns", "8.8.4.4",
        "--dns", "1.1.1.1"
    )

    foreach ($key in $Env.Keys) {
        $dockerArgs += @("-e", "${key}=$($Env[$key])")
    }

    $dockerArgs += @(
        "-v", "${ProjectRoot}:/build",
        "-v", "${InnerScriptPath}:/docker_build.sh",
        "-w", "/build",
        "ubuntu:22.04",
        "bash", "-c", "sed 's/\r$//' /docker_build.sh | bash"
    )

    $ErrorActionPreferenceBak = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & docker @dockerArgs 2>&1 | ForEach-Object { Write-Host $_ }
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = $ErrorActionPreferenceBak
    return $exitCode
}
