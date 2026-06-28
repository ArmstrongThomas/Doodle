param(
    [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$AppVersion = '0.0.0',
    [int]$TestMode = 0,
    [string]$AppTitle = 'Collab Doodle',
    [string]$AppDescription = 'Shared canvas drawing for Nintendo 3DS',
    [string]$AppAuthor = 'Tommy'
)

$ErrorActionPreference = 'Stop'

$makerom = Get-Command makerom.exe -ErrorAction SilentlyContinue
if (-not $makerom) {
    $candidate = 'C:\devkitPro\tools\bin\makerom.exe'
    if (Test-Path $candidate) {
        $makerom = [pscustomobject]@{ Source = $candidate }
    }
}

if (-not $makerom) {
    throw "makerom.exe was not found. Install the devkitPro 3DS CIA packaging tools, then rerun: make cia"
}

$bannertool = Get-Command bannertool.exe -ErrorAction SilentlyContinue
if (-not $bannertool) {
    $candidate = 'C:\devkitPro\tools\bin\bannertool.exe'
    if (Test-Path $candidate) {
        $bannertool = [pscustomobject]@{ Source = $candidate }
    }
}

if (-not $bannertool) {
    throw "bannertool.exe was not found. Install bannertool, then rerun: make cia"
}

$elf = Join-Path $ProjectRoot 'Doodle.elf'
$smdh = Join-Path $ProjectRoot 'Doodle.smdh'
$rsf = Join-Path $ProjectRoot 'cia.rsf'
$tempRsf = Join-Path $ProjectRoot 'build\cia.generated.rsf'
$bannerPng = Join-Path $ProjectRoot 'build\banner.png'
$bannerWav = Join-Path $ProjectRoot 'build\banner.wav'
$bannerBin = Join-Path $ProjectRoot 'build\banner.bin'
$iconBin = Join-Path $ProjectRoot 'build\icon.bin'
$cia = Join-Path $ProjectRoot 'Doodle.cia'

foreach ($required in @($elf, $smdh, $rsf)) {
    if (-not (Test-Path $required)) {
        throw "Required CIA input is missing: $required"
    }
}

$uniqueId = if ($TestMode -eq 1) { '0xCE476' } else { '0xCE475' }
$productCode = if ($TestMode -eq 1) { 'CTR-P-CDT1' } else { 'CTR-P-CDDL' }
$rsfText = [System.IO.File]::ReadAllText($rsf)
$rsfText = $rsfText -replace 'ProductCode\s+: "CTR-P-[^"]+"', "ProductCode             : `"$productCode`""
$rsfText = $rsfText -replace 'UniqueId\s+: 0x[0-9A-Fa-f]+', "UniqueId                : $uniqueId"
[System.IO.Directory]::CreateDirectory((Split-Path -Parent $tempRsf)) | Out-Null
$utf8NoBom = New-Object System.Text.UTF8Encoding $false
[System.IO.File]::WriteAllText($tempRsf, $rsfText, $utf8NoBom)

Add-Type -AssemblyName System.Drawing
$bitmap = New-Object System.Drawing.Bitmap 256, 128
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.Clear([System.Drawing.Color]::FromArgb(24, 33, 38))
$fontTitle = New-Object System.Drawing.Font 'Arial', 24, ([System.Drawing.FontStyle]::Bold)
$fontSub = New-Object System.Drawing.Font 'Arial', 12, ([System.Drawing.FontStyle]::Regular)
$brushWhite = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(245, 248, 250))
$brushAccent = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(94, 234, 212))
$graphics.DrawString('Collab Doodle', $fontTitle, $brushWhite, 18, 28)
$graphics.DrawString("v$AppVersion", $fontSub, $brushAccent, 20, 68)
$graphics.DrawString('Shared canvas for 3DS', $fontSub, $brushWhite, 20, 88)
$graphics.Dispose()
$bitmap.Save($bannerPng, [System.Drawing.Imaging.ImageFormat]::Png)
$bitmap.Dispose()

$sampleRate = 8000
$seconds = 1
$samples = $sampleRate * $seconds
$dataSize = $samples * 2
$writer = New-Object System.IO.BinaryWriter([System.IO.File]::Open($bannerWav, [System.IO.FileMode]::Create))
$writer.Write([System.Text.Encoding]::ASCII.GetBytes('RIFF'))
$writer.Write([int](36 + $dataSize))
$writer.Write([System.Text.Encoding]::ASCII.GetBytes('WAVEfmt '))
$writer.Write([int]16)
$writer.Write([int16]1)
$writer.Write([int16]1)
$writer.Write([int]$sampleRate)
$writer.Write([int]($sampleRate * 2))
$writer.Write([int16]2)
$writer.Write([int16]16)
$writer.Write([System.Text.Encoding]::ASCII.GetBytes('data'))
$writer.Write([int]$dataSize)
for ($i = 0; $i -lt $samples; $i++) {
    $writer.Write([int16]0)
}
$writer.Close()

& $bannertool.Source makebanner -i $bannerPng -a $bannerWav -o $bannerBin
if ($LASTEXITCODE -ne 0) {
    throw "bannertool makebanner failed with exit code $LASTEXITCODE"
}

& $bannertool.Source makesmdh -s "$AppTitle" -l "$AppDescription" -p "$AppAuthor" -i (Join-Path $ProjectRoot 'icon.png') -o $iconBin
if ($LASTEXITCODE -ne 0) {
    throw "bannertool makesmdh failed with exit code $LASTEXITCODE"
}

& $makerom.Source `
    -f cia `
    -target t `
    -exefslogo `
    -o $cia `
    -elf $elf `
    -rsf $tempRsf `
    -banner $bannerBin `
    -icon $iconBin

if ($LASTEXITCODE -ne 0) {
    throw "makerom failed with exit code $LASTEXITCODE"
}

Write-Host "Built CIA: $cia"
