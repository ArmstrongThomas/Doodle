param(
    [string]$Address = '192.168.1.225',
    [int]$Retries = 10,
    [ValidateSet(1, 2)]
    [int]$TestMode = 1,
    [string]$ServerHost = '',
    [switch]$Build
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$env:DEVKITPRO = 'C:\devkitPro'
$env:DEVKITARM = 'C:\devkitPro\devkitARM'
$env:PATH = 'C:\devkitPro\msys2\usr\bin;C:\devkitPro\tools\bin;C:\devkitPro\devkitARM\bin;' + $env:PATH

$make = 'C:\devkitPro\msys2\usr\bin\make.exe'
$link = 'C:\devkitPro\tools\bin\3dslink.exe'
$testBuildName = if ($TestMode -eq 2) { 'CollabDoodle-test-server2.3dsx' } else { 'CollabDoodle-test-local.3dsx' }
$testBuild = Join-Path $root $testBuildName

Push-Location $root
try {
    if ($Build -or -not (Test-Path $testBuild)) {
        & $make clean
        if ($ServerHost) {
            & $make TEST_MODE=$TestMode SERVER_HOST=$ServerHost
        } else {
            & $make TEST_MODE=$TestMode
        }
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
