param(
    [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$ServerRoot = (Resolve-Path (Join-Path (Split-Path -Parent $PSScriptRoot) '..\Doodle-Server')).Path,
    [string]$OutputDir = (Join-Path (Resolve-Path (Join-Path (Split-Path -Parent $PSScriptRoot) '..')).Path 'deploy'),
    [string]$OldVersion = '1.2.0',
    [string]$NewVersion = '1.2.1',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'

function Add-FileToBundle {
    param(
        [string]$Source,
        [string]$RelativePath
    )
    $target = Join-Path $bundleRoot $RelativePath
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
    Copy-Item -LiteralPath $Source -Destination $target -Force
}

if (-not $SkipBuild) {
    $stage = Join-Path $ProjectRoot 'scripts\stage-server2-test-build.ps1'
    & $stage -ProjectRoot $ProjectRoot -ServerRoot $ServerRoot -OldVersion $OldVersion -NewVersion $NewVersion | Write-Host
    if ($LASTEXITCODE -ne 0) {
        throw 'server2 staging failed'
    }
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$bundleRoot = Join-Path $OutputDir "server2-deploy-$stamp"
$zipPath = "$bundleRoot.zip"
$bootstrapSource = Join-Path $ServerRoot 'scripts\bootstrap-server2-bundle.ps1'
$bootstrapTarget = Join-Path $OutputDir "bootstrap-server2-bundle-$stamp.ps1"

if (Test-Path $bundleRoot) {
    Remove-Item -LiteralPath $bundleRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $bundleRoot | Out-Null

$serverFiles = @(
    'server.js',
    'package.json',
    'package-lock.json',
    'SERVER2.md',
    '.env.example'
)

foreach ($file in $serverFiles) {
    $source = Join-Path $ServerRoot $file
    if (Test-Path $source) {
        Add-FileToBundle $source $file
    }
}

foreach ($dir in @('src', 'scripts', 'public')) {
    $sourceDir = Join-Path $ServerRoot $dir
    if (-not (Test-Path $sourceDir)) { continue }
    Get-ChildItem -LiteralPath $sourceDir -Recurse -File | ForEach-Object {
        $relative = $_.FullName.Substring($ServerRoot.Length).TrimStart('\', '/')
        Add-FileToBundle $_.FullName $relative
    }
}

foreach ($file in @('latest.json', 'CollabDoodle.3dsx', 'CollabDoodle.cia')) {
    Add-FileToBundle (Join-Path $ServerRoot "updates\$file") "updates\$file"
}

$manifest = [ordered]@{
    generatedAt = (Get-Date).ToUniversalTime().ToString('o')
    server = 'server2.rpgwo.org'
    baseUrl = 'http://server2.rpgwo.org:3000'
    installUrl = 'http://server2.rpgwo.org:3000/builds/CollabDoodle-test-updater-old-server2.cia'
    oldVersion = $OldVersion
    newVersion = $NewVersion
    files = Get-ChildItem -LiteralPath $bundleRoot -Recurse -File | ForEach-Object {
        [ordered]@{
            path = $_.FullName.Substring($bundleRoot.Length).TrimStart('\', '/').Replace('\', '/')
            length = $_.Length
            sha256 = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
}

$manifestPath = Join-Path $bundleRoot 'deploy-manifest.json'
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

if (Test-Path $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}
Compress-Archive -Path (Join-Path $bundleRoot '*') -DestinationPath $zipPath -Force
if (Test-Path $bootstrapSource) {
    Copy-Item -LiteralPath $bootstrapSource -Destination $bootstrapTarget -Force
}

[pscustomobject]@{
    BundleDirectory = $bundleRoot
    ZipPath = $zipPath
    BootstrapScript = if (Test-Path $bootstrapTarget) { $bootstrapTarget } else { $null }
    InstallUrl = $manifest.installUrl
    FileCount = (Get-ChildItem -LiteralPath $bundleRoot -Recurse -File).Count
} | Format-List
