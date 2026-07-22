param(
    [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot),
    [int]$MaximumBundleAgeDays = 180
)

$ErrorActionPreference = 'Stop'

$versionHeader = Join-Path $ProjectRoot 'vendor\mbedtls\include\mbedtls\build_info.h'
$configPath = Join-Path $ProjectRoot 'include\doodle_mbedtls_config.h'
$bundlePath = Join-Path $ProjectRoot 'data\cacert.pem'
$hashPath = Join-Path $ProjectRoot 'vendor\ca-bundle\cacert.pem.sha256'

foreach ($requiredFile in @($versionHeader, $configPath, $bundlePath, $hashPath)) {
    if (-not (Test-Path -LiteralPath $requiredFile)) {
        throw "Missing TLS asset: $requiredFile"
    }
}

$version = [System.IO.File]::ReadAllText($versionHeader)
if ($version -notmatch '#define\s+MBEDTLS_VERSION_STRING\s+"3\.6\.7"') {
    throw 'Vendored Mbed TLS is not the pinned 3.6.7 release.'
}

$config = [System.IO.File]::ReadAllText($configPath)
$requiredDefines = @(
    'MBEDTLS_SSL_PROTO_TLS1_2',
    'MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED',
    'MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256',
    'MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256',
    'MBEDTLS_NO_PLATFORM_ENTROPY',
    'MBEDTLS_X509_CRT_PARSE_C'
)
foreach ($name in $requiredDefines) {
    if (-not $config.Contains($name)) {
        throw "Required TLS setting is absent: $name"
    }
}
if ($config -match '(?m)^\s*#define\s+MBEDTLS_SSL_PROTO_TLS1_3\b') {
    throw 'TLS 1.3 was enabled without extending the audited 3DS client configuration.'
}
if ($config -match '(?m)^\s*#define\s+MBEDTLS_KEY_EXCHANGE_(RSA|ECDHE_RSA)') {
    throw 'An unaudited RSA TLS key-exchange suite was enabled.'
}

$expectedHash = ([System.IO.File]::ReadAllText($hashPath).Trim() -split '\s+')[0].ToLowerInvariant()
$actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $bundlePath).Hash.ToLowerInvariant()
if ($expectedHash -ne $actualHash) {
    throw "Embedded CA bundle hash mismatch: expected $expectedHash, got $actualHash"
}

$bundle = [System.IO.File]::ReadAllText($bundlePath)
$certificateCount = ([regex]::Matches($bundle, '-----BEGIN CERTIFICATE-----')).Count
if ($certificateCount -lt 100) {
    throw "CA bundle is unexpectedly small ($certificateCount certificates)."
}
if ($bundle -notmatch 'Certificate data from Mozilla as of:\s*([^\r\n]+)') {
    throw 'CA bundle does not contain Mozilla extraction-date metadata.'
}
$bundleDate = [DateTime]::ParseExact($Matches[1], "ddd MMM dd HH:mm:ss yyyy 'GMT'",
    [Globalization.CultureInfo]::InvariantCulture,
    [Globalization.DateTimeStyles]::AssumeUniversal -bor
        [Globalization.DateTimeStyles]::AdjustToUniversal)
$ageDays = ([DateTime]::UtcNow - $bundleDate).TotalDays
if ($ageDays -lt -2 -or $ageDays -gt $MaximumBundleAgeDays) {
    throw "Mozilla CA bundle age is outside policy: $([math]::Round($ageDays, 1)) days."
}

Write-Host "TLS assets OK: Mbed TLS 3.6.7, $certificateCount Mozilla roots, SHA-256 $actualHash"
