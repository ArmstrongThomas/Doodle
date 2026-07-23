param(
    [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
$BuildDir = Join-Path $ProjectRoot 'build\host-tests'
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Build Tools were not found (vswhere.exe is missing).'
}

$installPath = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
if (-not $installPath) {
    throw 'Visual Studio C++ Build Tools were not found.'
}

$devShell = Join-Path $installPath 'Common7\Tools\VsDevCmd.bat'
$binary = Join-Path $BuildDir 'client_fixture_tests.exe'
$sources = @(
    (Join-Path $ProjectRoot 'tests\client_fixture_tests.cpp'),
    (Join-Path $ProjectRoot 'source\client_settings.cpp'),
    (Join-Path $ProjectRoot 'source\input_bindings.cpp'),
    (Join-Path $ProjectRoot 'source\protocol.cpp'),
    (Join-Path $ProjectRoot 'source\ui_canvas.cpp'),
    (Join-Path $ProjectRoot 'source\ui_route.cpp')
)
$quotedSources = ($sources | ForEach-Object { '"' + $_ + '"' }) -join ' '
$stubInclude = Join-Path $ProjectRoot 'tests\stubs'
$appInclude = Join-Path $ProjectRoot 'include'
$compat = Join-Path $stubInclude 'host_compat.h'

$command = @(
    'call "' + $devShell + '" -no_logo -arch=x64',
    '&& cd /d "' + $BuildDir + '"',
    '&&',
    'cl.exe /nologo /EHsc /std:c++14 /W4 /D_CRT_SECURE_NO_WARNINGS',
    '/FI"' + $compat + '"',
    '/I"' + $stubInclude + '"',
    '/I"' + $appInclude + '"',
    $quotedSources,
    '/Fe:"' + $binary + '"',
    '/link /INCREMENTAL:NO',
    '&& cd /d "' + $ProjectRoot + '"',
    '&& "' + $binary + '"'
) -join ' '

& $env:ComSpec /d /c $command
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
