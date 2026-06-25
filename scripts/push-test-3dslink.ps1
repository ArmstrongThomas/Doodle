param(
    [string]$Address = '192.168.1.225',
    [int]$Retries = 10,
    [string]$ServerHost = '192.168.1.46',
    [switch]$Build
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$env:DEVKITPRO = 'C:\devkitPro'
$env:DEVKITARM = 'C:\devkitPro\devkitARM'
$env:PATH = 'C:\devkitPro\msys2\usr\bin;C:\devkitPro\tools\bin;C:\devkitPro\devkitARM\bin;' + $env:PATH

$make = 'C:\devkitPro\msys2\usr\bin\make.exe'
$link = 'C:\devkitPro\tools\bin\3dslink.exe'
$testBuild = Join-Path $root 'CollabDoodle-test-local.3dsx'

Push-Location $root
try {
    if ($Build -or -not (Test-Path $testBuild)) {
        & $make clean
        & $make TEST_MODE=1 SERVER_HOST=$ServerHost
        Copy-Item -LiteralPath (Join-Path $root 'Doodle.3dsx') -Destination $testBuild -Force
    }

    if (-not (Test-Path $testBuild)) {
        throw "Test build not found: $testBuild"
    }

    Write-Output "Sending $testBuild to $Address with 3dslink"
    & $link -a $Address -r $Retries $testBuild
}
finally {
    Pop-Location
}
