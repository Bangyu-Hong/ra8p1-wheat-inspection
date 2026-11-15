param(
    [string] $RdpmDirectory = $env:RA8P1_RDPM_DIR,
    [switch] $Apply1024KiB
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($RdpmDirectory)) {
    $RdpmDirectory = "C:\Temp\rdpm_ra8p1_jlink962_x86"
}

$rdpm = Join-Path $RdpmDirectory "RenesasDevicePartitionManagerCmd.exe"
$jlinkDll = Join-Path $RdpmDirectory "JLinkARM.dll"

if (-not (Test-Path -LiteralPath $rdpm)) {
    throw "RDPM executable not found: $rdpm. Pass -RdpmDirectory with the isolated 32-bit RDPM/J-Link directory."
}

if (-not (Test-Path -LiteralPath $jlinkDll)) {
    throw "JLinkARM.dll not found beside RDPM: $jlinkDll. RDPM and the J-Link DLL must have matching bitness."
}

function Invoke-RdpmStatus {
    $statusOutput = & $rdpm -action status -emutype jlink -bootinterface swd 2>&1
    $exitCode = $LASTEXITCODE
    $statusText = $statusOutput -join [Environment]::NewLine
    Write-Host $statusText

    if (0 -ne $exitCode) {
        throw "RDPM status failed with exit code $exitCode."
    }

    $match = [regex]::Match($statusText, "Code MRAM Secure\s+\(kB\)\s*:\s*(\d+)")
    if (-not $match.Success) {
        throw "RDPM completed but the Code MRAM Secure size could not be parsed."
    }

    return [int] $match.Groups[1].Value
}

$secureKiB = Invoke-RdpmStatus
if (1024 -eq $secureKiB) {
    Write-Output "mram-boundary status=ok secure_kib=1024"
    exit 0
}

if (-not $Apply1024KiB) {
    throw "Code MRAM Secure is $secureKiB KiB, expected 1024 KiB. Re-run with -Apply1024KiB after confirming the target is the RA8P1 camera board."
}

Write-Output "mram-boundary action=expand from_kib=$secureKiB to_kib=1024"
& $rdpm -action boundary -emutype jlink -bootinterface swd -idaucfs 1024
if (0 -ne $LASTEXITCODE) {
    throw "RDPM boundary update failed with exit code $LASTEXITCODE."
}

$verifiedKiB = Invoke-RdpmStatus
if (1024 -ne $verifiedKiB) {
    throw "RDPM boundary update did not persist: expected 1024 KiB, read $verifiedKiB KiB."
}

Write-Output "mram-boundary status=updated secure_kib=1024"
