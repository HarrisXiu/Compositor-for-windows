param(
    # Copies the verified archives here, with SHA256SUMS.txt, for publishing beside a release.
    [string]$Destination
)
$ErrorActionPreference = 'Stop'
# Corresponding source for the Qt 6.10.2 libraries the portable package deploys (LGPLv3).
# Hashes were checked against Qt's published md5sums.txt for 6.10.2.
$taskQtSources = [ordered]@{
    'qtbase-everywhere-src-6.10.2.tar.xz' = 'AEB78D29291A2B5FD53CB55950F8F5065B4978C25FB1D77F627D695AB9ADF21E'
    'qtsvg-everywhere-src-6.10.2.tar.xz' = 'F07FF80F38CAF235187200345392CA7479445DDF49A36C3694CD52A735DAD6E1'
    'qtimageformats-everywhere-src-6.10.2.tar.xz' = '8B8F9C718638081E7B3C000E7F31910140B1202A98E98DF5D1B496FE6F639D67'
}
$taskCache = Join-Path (Split-Path -Parent $PSScriptRoot) '.cache\qt-source'
New-Item -ItemType Directory -Path $taskCache -Force | Out-Null
foreach ($taskName in $taskQtSources.Keys) {
    $taskPath = Join-Path $taskCache $taskName
    if (-not (Test-Path -LiteralPath $taskPath)) {
        Invoke-WebRequest -Uri "https://download.qt.io/archive/qt/6.10/6.10.2/submodules/$taskName" -OutFile ($taskPath + '.download')
        Move-Item -LiteralPath ($taskPath + '.download') -Destination $taskPath -Force
    }
    if ((Get-FileHash -LiteralPath $taskPath -Algorithm SHA256).Hash -ne $taskQtSources[$taskName]) { throw "Qt source archive SHA256 mismatch: $taskName" }
}
if ($Destination) {
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    foreach ($taskName in $taskQtSources.Keys) { Copy-Item -LiteralPath (Join-Path $taskCache $taskName) -Destination $Destination -Force }
    $taskQtSources.Keys | ForEach-Object { "$($taskQtSources[$_].ToLowerInvariant())  $_" } | Set-Content -LiteralPath (Join-Path $Destination 'SHA256SUMS.txt') -Encoding ascii
}
$taskCache
