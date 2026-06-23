param(
    [string]$Address = '192.168.1.225',
    [int]$Retries = 10
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$env:DEVKITPRO = 'C:\devkitPro'
$env:DEVKITARM = 'C:\devkitPro\devkitARM'
$env:PATH = 'C:\devkitPro\msys2\usr\bin;C:\devkitPro\tools\bin;C:\devkitPro\devkitARM\bin;' + $env:PATH

Push-Location $root
try {
    & 'C:\devkitPro\msys2\usr\bin\make.exe' clean
    & 'C:\devkitPro\msys2\usr\bin\make.exe'
    $updatesDir = Join-Path (Split-Path -Parent $root) 'Doodle-Server\updates'
    New-Item -ItemType Directory -Force -Path $updatesDir | Out-Null
    Copy-Item -Force -Path (Join-Path $root 'Doodle.3dsx') -Destination (Join-Path $updatesDir 'CollabDoodle.3dsx')
    & 'C:\devkitPro\tools\bin\3dslink.exe' -a $Address -r $Retries Doodle.3dsx
}
finally {
    Pop-Location
}
