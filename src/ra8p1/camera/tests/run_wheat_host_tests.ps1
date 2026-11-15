$ErrorActionPreference = "Stop"

$cameraRoot = Split-Path -Parent $PSScriptRoot
$python = "E:\Anaconda\anaconda_install_way\envs\yolo\python.exe"

if (-not (Test-Path -LiteralPath $python)) {
    $python = (Get-Command python.exe -ErrorAction Stop).Source
}

$env:PYTHONPATH = Join-Path $cameraRoot "tools"
& $python -m unittest discover -s $PSScriptRoot -p "test_wheat*.py"
if ($LASTEXITCODE -ne 0) {
    throw "Wheat host reference tests failed with exit code $LASTEXITCODE"
}

Write-Output "wheat-host-tests status=ok"
