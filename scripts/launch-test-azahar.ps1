param(
    [ValidateSet(1, 2)]
    [int]$TestMode = 1,
    [string]$ServerHost = '',
    [string]$AzaharPath = 'C:\Program Files\Azahar\azahar.exe',
    [switch]$Build
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$env:DEVKITPRO = 'C:\devkitPro'
$env:DEVKITARM = 'C:\devkitPro\devkitARM'
$env:PATH = 'C:\devkitPro\msys2\usr\bin;C:\devkitPro\tools\bin;C:\devkitPro\devkitARM\bin;' + $env:PATH

$make = 'C:\devkitPro\msys2\usr\bin\make.exe'
$testBuildName = if ($TestMode -eq 2) { 'CollabDoodle-test-server2.3dsx' } else { 'CollabDoodle-test-local.3dsx' }
$testBuild = Join-Path $root $testBuildName

if (-not (Test-Path -LiteralPath $AzaharPath)) {
    throw "Azahar was not found at: $AzaharPath"
}

Push-Location $root
try {
    if ($Build -or -not (Test-Path -LiteralPath $testBuild)) {
        & $make clean
        if ($ServerHost) {
            & $make TEST_MODE=$TestMode SERVER_HOST=$ServerHost
        } else {
            & $make TEST_MODE=$TestMode
        }
        Copy-Item -LiteralPath (Join-Path $root 'Doodle.3dsx') -Destination $testBuild -Force
    }

    if (-not (Test-Path -LiteralPath $testBuild)) {
        throw "Test build not found: $testBuild"
    }

    Write-Host "Launching $testBuild in Azahar"
    Start-Process -FilePath $AzaharPath -ArgumentList "`"$testBuild`""
}
finally {
    Pop-Location
}
