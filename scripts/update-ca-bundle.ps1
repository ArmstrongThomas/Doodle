param(
    [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'

$bundleUrl = 'https://curl.se/ca/cacert.pem'
$checksumUrl = 'https://curl.se/ca/cacert.pem.sha256'
$dataDir = Join-Path $ProjectRoot 'data'
$destination = Join-Path $dataDir 'cacert.pem'
$temporary = Join-Path $dataDir 'cacert.pem.download'
$metadataDir = Join-Path $ProjectRoot 'vendor\ca-bundle'
$checksumDestination = Join-Path $metadataDir 'cacert.pem.sha256'

[System.IO.Directory]::CreateDirectory($dataDir) | Out-Null
[System.IO.Directory]::CreateDirectory($metadataDir) | Out-Null

try {
    Invoke-WebRequest -UseBasicParsing -Uri $bundleUrl -OutFile $temporary
    $checksumResponse = Invoke-WebRequest -UseBasicParsing -Uri $checksumUrl
    $checksumText = if ($checksumResponse.Content -is [byte[]]) {
        [System.Text.Encoding]::ASCII.GetString($checksumResponse.Content).Trim()
    } else {
        ([string]$checksumResponse.Content).Trim()
    }
    $expected = ($checksumText -split '\s+')[0].ToLowerInvariant()
    if ($expected -notmatch '^[0-9a-f]{64}$') {
        throw "Unexpected checksum response from $checksumUrl"
    }

    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $temporary).Hash.ToLowerInvariant()
    if ($actual -ne $expected) {
        throw "CA bundle checksum mismatch: expected $expected, got $actual"
    }

    Move-Item -LiteralPath $temporary -Destination $destination -Force
    [System.IO.File]::WriteAllText($checksumDestination, "$actual  cacert.pem`n",
        (New-Object System.Text.UTF8Encoding $false))
    Write-Host "Updated Mozilla CA bundle: $destination"
    Write-Host "SHA-256: $actual"
}
finally {
    if (Test-Path -LiteralPath $temporary) {
        Remove-Item -LiteralPath $temporary -Force
    }
}
