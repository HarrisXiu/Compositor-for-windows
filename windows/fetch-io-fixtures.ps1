# SPDX-License-Identifier: MIT
param([string]$Output = (Join-Path (Split-Path $PSScriptRoot -Parent) 'artifacts/io1-io5/fixtures'))
$ErrorActionPreference = 'Stop'
$taskPsd = 'https://raw.githubusercontent.com/Agamnentzar/ag-psd/387049670cb89b88fb8fe1b7c01aeacf98dd2e3b'
$taskHeif = 'https://raw.githubusercontent.com/strukturag/libheif/e981ebdf2b46a761820d41150ae8154d9dcc9e52'
$taskFixtures = @(
    @('effects-src.psd', "$taskPsd/test/read/effects/src.psd", 'b7f467529c585537fd982ab9c0879b18b0d05340e72b06d705b324c11fb00d25'),
    @('effects-reference.png', "$taskPsd/test/read/effects/canvas.png", '1c1ab8ea4165ce00dc7867a04b373e1572149bc7791ff29825e066e9a4363826'),
    @('vector-complex-src.psd', "$taskPsd/test/read/vector-complex/src.psd", '80b841f3458fb9551fa2ada7b38168ab395c23f4e2ed2124f4df4d21f34b320f'),
    @('vector-complex-reference.png', "$taskPsd/test/read/vector-complex/canvas.png", 'c150db49ae0b3f462e957a0d369536fe6cbc4c481ab6584776181a95cdf01141'),
    @('vector-layer-src.psd', "$taskPsd/test/read/vector-layer/src.psd", 'dd76c94c575e735191ded3858c494f2c36334f21e6a079da8086677a007c7ff5'),
    @('vector-layer-reference.png', "$taskPsd/test/read/vector-layer/canvas.png", '54a7f562a079cb586862fae68b26b57d99d07df215d786dd1ac6d60df0693a7b'),
    @('winding-even-odd-src.psd', "$taskPsd/test/read/winding-even-odd/src.psd", '863d4294dee03bf6fe3ea23448e0e70b0b943804dd6d63b1f531756e288d7d36'),
    @('winding-even-odd-reference.png', "$taskPsd/test/read/winding-even-odd/canvas.png", 'f305f32150b1f84cdb22210b533b98102be031216417d874eea6764be062786c'),
    @('winding-non-zero-src.psd', "$taskPsd/test/read/winding-non-zero/src.psd", '3331bfb5dd9712d1666874fa5b797ad6debc3b8587674126220a6a5d1fa4eb76'),
    @('winding-non-zero-reference.png', "$taskPsd/test/read/winding-non-zero/canvas.png", 'edc509f6365c70d5794b3e56fa4317525118a05c25ccdb20528ce21a9fbd77ed'),
    @('psb-test-src.psb', "$taskPsd/test/read/psb-test/src.psb", '34765a16f24304b0f459380327023de122c8d9e62e78b092efa742cb1173cbe9'),
    @('ag-psd-LICENSE', "$taskPsd/LICENSE", '035430250509b801de02201d2f9bc9388a1220198f9276d38f4059df882e585c'),
    @('example.heic', "$taskHeif/example.heic", 'c6a1e7cba3ff88c6ddc41b4269081dedb76ab6e82e5bb1ffac98154aa1c6da56')
)
$null = New-Item -ItemType Directory -Path $Output -Force
foreach ($taskFixture in $taskFixtures) {
    $taskPath = Join-Path $Output $taskFixture[0]
    if ((Test-Path -LiteralPath $taskPath) -and
        (Get-FileHash -LiteralPath $taskPath -Algorithm SHA256).Hash -eq $taskFixture[2]) {
        continue
    }
    $taskTemporary = $taskPath + '.download'
    Invoke-WebRequest -Uri $taskFixture[1] -OutFile $taskTemporary -UseBasicParsing
    if ((Get-FileHash -LiteralPath $taskTemporary -Algorithm SHA256).Hash -ne $taskFixture[2]) {
        throw ('Fixture SHA256 mismatch: ' + $taskFixture[0])
    }
    Move-Item -LiteralPath $taskTemporary -Destination $taskPath -Force
}
$taskFixtures | ForEach-Object {
    [PSCustomObject]@{ file = $_[0]; url = $_[1]; sha256 = $_[2] }
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Output 'sources.json') -Encoding utf8
Write-Host ('IO fixtures verified: ' + [IO.Path]::GetFullPath($Output))
