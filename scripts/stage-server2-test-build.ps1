param(
    [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$ServerRoot = (Resolve-Path (Join-Path (Split-Path -Parent $PSScriptRoot) '..\Doodle-Server')).Path,
    [string]$OldVersion = '1.2.0',
    [string]$NewVersion = '1.2.1'
)

$ErrorActionPreference = 'Stop'

$env:DEVKITPRO = if ($env:DEVKITPRO) { $env:DEVKITPRO } else { 'C:\devkitPro' }
$env:DEVKITARM = if ($env:DEVKITARM) { $env:DEVKITARM } else { 'C:\devkitPro\devkitARM' }
$env:PATH = "C:\devkitPro\msys2\usr\bin;C:\devkitPro\tools\bin;C:\devkitPro\devkitARM\bin;$env:PATH"

$prep = Join-Path $ProjectRoot 'scripts\prepare-cia-self-update-test.ps1'
& $prep -TestMode 2 -OldVersion $OldVersion -NewVersion $NewVersion -PublishToLocalServer -ProjectRoot $ProjectRoot -ServerRoot $ServerRoot
if ($LASTEXITCODE -ne 0) {
    throw 'server2 test build preparation failed'
}

$publicBuilds = Join-Path $ServerRoot 'public\builds'
New-Item -ItemType Directory -Force -Path $publicBuilds | Out-Null

$oldCia = Join-Path $ProjectRoot 'CollabDoodle-test-updater-old-server2.cia'
$updateCia = Join-Path $ProjectRoot 'CollabDoodle-test-updater-update-server2.cia'
$test3dsx = Join-Path $ProjectRoot 'CollabDoodle-test-updater-update-server2.3dsx'

Copy-Item -LiteralPath $oldCia -Destination (Join-Path $publicBuilds 'CollabDoodle-test-updater-old-server2.cia') -Force
Copy-Item -LiteralPath $updateCia -Destination (Join-Path $publicBuilds 'CollabDoodle-test-updater-update-server2.cia') -Force
Copy-Item -LiteralPath $updateCia -Destination (Join-Path $publicBuilds 'CollabDoodle-test-server2.cia') -Force
Copy-Item -LiteralPath $test3dsx -Destination (Join-Path $publicBuilds 'CollabDoodle-test-server2.3dsx') -Force

$indexPath = Join-Path $publicBuilds 'index.html'
$installUrl = 'http://server2.rpgwo.org:3000/builds/CollabDoodle-test-updater-old-server2.cia'
$qrData = [uri]::EscapeDataString($installUrl)
$html = @"
<!doctype html>
<html lang="en">
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Collab Doodle Server2 Test Build</title>
    <style>
        :root { --bg:#eef2f4; --panel:#fff; --ink:#20242a; --muted:#687280; --line:#c9d3dc; --accent:#0d7a75; }
        * { box-sizing:border-box; }
        body { margin:0; min-height:100vh; display:grid; place-items:center; padding:24px; background:var(--bg); color:var(--ink); font-family:Arial, Helvetica, sans-serif; }
        main { width:min(100%, 520px); padding:22px; background:var(--panel); border:1px solid var(--line); border-radius:8px; }
        h1 { margin:0 0 8px; font-size:22px; }
        p { margin:8px 0; color:var(--muted); }
        .qr { display:block; width:min(100%, 320px); height:auto; margin:18px auto; border:1px solid var(--line); background:#fff; image-rendering:pixelated; }
        code { display:block; overflow-wrap:anywhere; padding:10px; border:1px solid var(--line); border-radius:6px; background:#f8fafb; color:var(--ink); }
        a { color:var(--accent); font-weight:700; }
        .links { display:grid; gap:8px; margin-top:16px; }
    </style>
</head>
<body>
<main>
    <h1>Collab Doodle Server2 Test Build</h1>
    <p>Scan with FBI remote install:</p>
    <img class="qr" alt="QR code for Collab Doodle test CIA" src="https://api.qrserver.com/v1/create-qr-code/?size=320x320&amp;margin=12&amp;data=$qrData">
    <code>$installUrl</code>
    <div class="links">
        <a href="CollabDoodle-test-updater-old-server2.cia">Install old updater-test CIA</a>
        <a href="CollabDoodle-test-updater-update-server2.cia">Download update CIA</a>
        <a href="CollabDoodle-test-server2.cia">Download latest test CIA</a>
        <a href="CollabDoodle-test-server2.3dsx">Download 3DSX</a>
    </div>
</main>
</body>
</html>
"@
$html | Set-Content -LiteralPath $indexPath -Encoding UTF8

$updatesDir = Join-Path $ServerRoot 'updates'
$summary = [ordered]@{
    installUrl = $installUrl
    copyToServer2 = @(
        'updates\latest.json',
        'updates\CollabDoodle.3dsx',
        'updates\CollabDoodle.cia',
        'public\builds\index.html',
        'public\builds\CollabDoodle-test-updater-old-server2.cia',
        'public\builds\CollabDoodle-test-updater-update-server2.cia',
        'public\builds\CollabDoodle-test-server2.cia',
        'public\builds\CollabDoodle-test-server2.3dsx'
    )
    files = @(
        (Get-Item (Join-Path $updatesDir 'latest.json')),
        (Get-Item (Join-Path $updatesDir 'CollabDoodle.3dsx')),
        (Get-Item (Join-Path $updatesDir 'CollabDoodle.cia')),
        (Get-Item (Join-Path $publicBuilds 'index.html')),
        (Get-Item (Join-Path $publicBuilds 'CollabDoodle-test-updater-old-server2.cia')),
        (Get-Item (Join-Path $publicBuilds 'CollabDoodle-test-updater-update-server2.cia')),
        (Get-Item (Join-Path $publicBuilds 'CollabDoodle-test-server2.cia')),
        (Get-Item (Join-Path $publicBuilds 'CollabDoodle-test-server2.3dsx'))
    ) | ForEach-Object {
        [ordered]@{
            path = $_.FullName
            length = $_.Length
            sha256 = if ($_.Extension -in @('.cia', '.3dsx', '.json')) { (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() } else { $null }
        }
    }
}

$summary | ConvertTo-Json -Depth 5
