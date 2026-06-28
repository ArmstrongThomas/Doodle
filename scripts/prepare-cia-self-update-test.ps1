param(
    [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$ServerRoot = (Resolve-Path (Join-Path (Split-Path -Parent $PSScriptRoot) '..\Doodle-Server')).Path,
    [ValidateSet(1, 2)]
    [int]$TestMode = 1,
    [string]$ServerHost = '',
    [string]$OldVersion = '1.2.0',
    [string]$NewVersion = '1.2.1',
    [switch]$PublishToLocalServer
)

$ErrorActionPreference = 'Stop'

$env:DEVKITPRO = if ($env:DEVKITPRO) { $env:DEVKITPRO } else { 'C:\devkitPro' }
$env:DEVKITARM = if ($env:DEVKITARM) { $env:DEVKITARM } else { 'C:\devkitPro\devkitARM' }
$env:PATH = "C:\devkitPro\msys2\usr\bin;C:\devkitPro\tools\bin;C:\devkitPro\devkitARM\bin;$env:PATH"

function Invoke-Make {
    param(
        [Parameter(ValueFromRemainingArguments = $true)]
        [string[]]$Arguments
    )
    Push-Location $ProjectRoot
    try {
        & make @Arguments
        if ($LASTEXITCODE -ne 0) {
            throw "make failed: $($Arguments -join ' ')"
        }
    } finally {
        Pop-Location
    }
}

function Copy-Build($Name) {
    Copy-Item (Join-Path $ProjectRoot 'Doodle.3dsx') (Join-Path $ProjectRoot "$Name.3dsx") -Force
    Copy-Item (Join-Path $ProjectRoot 'Doodle.cia') (Join-Path $ProjectRoot "$Name.cia") -Force
}

function Resolve-TestServerHost {
    if ($ServerHost) {
        return $ServerHost
    }
    if ($TestMode -eq 2) {
        return 'server2.rpgwo.org'
    }
    return '192.168.1.46'
}

function Build-TestUpdater($Version, $Name) {
    $previousAppVersion = $env:APP_VERSION
    $previousServerHost = $env:SERVER_HOST
    $previousTestMode = $env:TEST_MODE
    $previousDisableUpdater = $env:DISABLE_UPDATER
    try {
        $env:APP_VERSION = $Version
        $env:SERVER_HOST = Resolve-TestServerHost
        $env:TEST_MODE = [string]$TestMode
        $env:DISABLE_UPDATER = '0'
        Invoke-Make @('clean')
        Invoke-Make @()
        Invoke-Make @('cia')
        Copy-Build $Name
    } finally {
        $env:APP_VERSION = $previousAppVersion
        $env:SERVER_HOST = $previousServerHost
        $env:TEST_MODE = $previousTestMode
        $env:DISABLE_UPDATER = $previousDisableUpdater
    }
}

$suffix = if ($TestMode -eq 2) { 'server2' } else { 'local' }
$oldName = "CollabDoodle-test-updater-old-$suffix"
$updateName = "CollabDoodle-test-updater-update-$suffix"

Build-TestUpdater $OldVersion $oldName
Build-TestUpdater $NewVersion $updateName

if ($PublishToLocalServer) {
    $updatesDir = Join-Path $ServerRoot 'updates'
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    foreach ($file in @('latest.json', 'CollabDoodle.3dsx', 'CollabDoodle.cia')) {
        $path = Join-Path $updatesDir $file
        if (Test-Path $path) {
            Copy-Item $path "$path.$stamp.bak" -Force
        }
    }

    Copy-Item (Join-Path $ProjectRoot "$updateName.3dsx") (Join-Path $updatesDir 'CollabDoodle.3dsx') -Force
    Copy-Item (Join-Path $ProjectRoot "$updateName.cia") (Join-Path $updatesDir 'CollabDoodle.cia') -Force

    $three = Get-Item (Join-Path $updatesDir 'CollabDoodle.3dsx')
    $cia = Get-Item (Join-Path $updatesDir 'CollabDoodle.cia')
    $threeHash = (Get-FileHash $three.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    $ciaHash = (Get-FileHash $cia.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    $manifest = [ordered]@{
        type = 'updateManifest'
        appId = 'collab-doodle'
        latestVersion = $NewVersion
        minSupportedVersion = $OldVersion
        releaseNotes = 'Local test-title CIA self-update package.'
        selectedPackage = '3dsx'
        artifactType = '3dsx'
        artifactUrl = $null
        artifactName = 'CollabDoodle.3dsx'
        artifactSize = $three.Length
        sha256 = $threeHash
        artifacts = [ordered]@{
            '3dsx' = [ordered]@{
                artifactType = '3dsx'
                artifactName = 'CollabDoodle.3dsx'
                artifactSize = $three.Length
                sha256 = $threeHash
            }
            cia = [ordered]@{
                artifactType = 'cia'
                artifactName = 'CollabDoodle.cia'
                artifactSize = $cia.Length
                sha256 = $ciaHash
            }
        }
        generatedAt = (Get-Date).ToUniversalTime().ToString('o')
    }
    $manifest | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $updatesDir 'latest.json') -Encoding UTF8
}

Get-Item `
    (Join-Path $ProjectRoot "$oldName.cia"), `
    (Join-Path $ProjectRoot "$updateName.cia") |
    Select-Object Name, Length
