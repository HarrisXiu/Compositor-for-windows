param(
    [string]$QtRoot = $env:QT_ROOT_DIR,
    [string]$LibRawRoot,
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [switch]$Package,
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$taskProjectRoot = Split-Path -Parent $PSScriptRoot
if (-not $QtRoot) { $QtRoot = Join-Path $taskProjectRoot '.cache\Qt\6.10.2\msvc2022_64' }
$QtRoot = [System.IO.Path]::GetFullPath($QtRoot)
if (-not (Test-Path -LiteralPath (Join-Path $QtRoot 'lib\cmake\Qt6\Qt6Config.cmake'))) {
    throw 'Qt 6.8+ development libraries are required. Pass -QtRoot C:\Qt\6.10.2\msvc2022_64. See windows/README.md.'
}
$taskCMakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
if ($taskCMakeCommand) { $taskCMake = $taskCMakeCommand.Source } else {
    $taskVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $taskVswhere)) { throw 'Install Visual Studio C++ build tools and CMake.' }
    $taskVsRoot = & $taskVswhere -products '*' -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $taskCMake = Join-Path $taskVsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}
if (-not (Test-Path -LiteralPath $taskCMake)) { throw 'CMake executable not found.' }
$taskBuildRoot = Join-Path $taskProjectRoot 'build\windows'
$taskLibRawArchive = Join-Path $taskProjectRoot '.cache\LibRaw-0.22.2-Win64.zip'
if (-not $LibRawRoot) {
    $LibRawRoot = Join-Path $taskProjectRoot '.cache\LibRaw\LibRaw-0.22.2'
    $taskLibRawHash = 'AC64FA12BB00A7581332D4C6AB918C0533FB3F119D6B668D47A6875410DCA948'
    if (-not (Test-Path -LiteralPath $taskLibRawArchive)) {
        New-Item -ItemType Directory -Path (Split-Path -Parent $taskLibRawArchive) -Force | Out-Null
        Invoke-WebRequest -Uri 'https://www.libraw.org/data/LibRaw-0.22.2-Win64.zip' -OutFile ($taskLibRawArchive + '.download')
        if ((Get-FileHash -LiteralPath ($taskLibRawArchive + '.download') -Algorithm SHA256).Hash -ne $taskLibRawHash) { throw 'LibRaw archive SHA256 mismatch.' }
        Move-Item -LiteralPath ($taskLibRawArchive + '.download') -Destination $taskLibRawArchive -Force
    }
    if ((Get-FileHash -LiteralPath $taskLibRawArchive -Algorithm SHA256).Hash -ne $taskLibRawHash) { throw 'LibRaw archive SHA256 mismatch.' }
    if (-not (Test-Path -LiteralPath (Join-Path $LibRawRoot 'lib\libraw.lib'))) {
        Expand-Archive -LiteralPath $taskLibRawArchive -DestinationPath (Split-Path -Parent $LibRawRoot) -Force
    }
}
& $taskCMake -S $taskProjectRoot -B $taskBuildRoot -A x64 "-DCMAKE_PREFIX_PATH=$QtRoot" "-DCOMPOSITOR_LIBRAW_ROOT=$LibRawRoot" -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& $taskCMake --build $taskBuildRoot --config $Configuration --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'C++ build failed.' }
$taskPreviousPath = $env:PATH
try {
    $env:PATH = (Join-Path $QtRoot 'bin') + ';' + $env:PATH
    if (-not $SkipTests) {
        $taskCTest = Join-Path (Split-Path -Parent $taskCMake) 'ctest.exe'
        & $taskCTest --test-dir $taskBuildRoot -C $Configuration --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
    }
    if ($Package) {
        if ($Configuration -ne 'Release') { throw 'Use Release configuration for packaging.' }
        if ((Get-FileHash -LiteralPath (Join-Path $LibRawRoot 'bin\libraw.dll') -Algorithm SHA256).Hash -ne '6A459C22039ABF0EAC4D263673337C8ED5F223ACBD372FCF77610DEBF80AC8CD') { throw 'This packaging script requires the pinned LibRaw SDK DLL so its bundled source matches.' }
        if ((Get-FileHash -LiteralPath $taskLibRawArchive -Algorithm SHA256).Hash -ne 'AC64FA12BB00A7581332D4C6AB918C0533FB3F119D6B668D47A6875410DCA948') { throw 'LibRaw source archive SHA256 mismatch.' }
        $taskArtifactRoot = Join-Path $taskProjectRoot 'artifacts'
        $taskPackagePath = Join-Path $taskArtifactRoot 'Compositor-Windows-x64'
        New-Item -ItemType Directory -Path $taskPackagePath -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $taskBuildRoot "$Configuration\Compositor.exe") -Destination $taskPackagePath -Force
        Copy-Item -LiteralPath (Join-Path $LibRawRoot 'bin\libraw.dll') -Destination $taskPackagePath -Force
        $taskThirdPartySource = Join-Path $taskPackagePath 'third-party-source'
        New-Item -ItemType Directory -Path $taskThirdPartySource -Force | Out-Null
        if (-not (Test-Path -LiteralPath $taskLibRawArchive)) { throw 'Include the matching LibRaw source archive for redistribution.' }
        Copy-Item -LiteralPath $taskLibRawArchive -Destination $taskThirdPartySource -Force
        & (Join-Path $QtRoot 'bin\windeployqt.exe') --release --no-translations --compiler-runtime --no-opengl-sw $taskPackagePath
        if ($LASTEXITCODE -ne 0) { throw 'Qt deployment failed.' }
        if (-not $taskVsRoot) {
            $taskVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
            if (Test-Path -LiteralPath $taskVswhere) { $taskVsRoot = & $taskVswhere -products '*' -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
        }
        if ($taskVsRoot) {
            $taskRedist = Get-ChildItem -LiteralPath (Join-Path $taskVsRoot 'VC\Redist\MSVC') -Directory | Where-Object Name -Match '^\d+\.' | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
            $taskCrt = Get-ChildItem -LiteralPath (Join-Path $taskRedist.FullName 'x64') -Directory -Filter 'Microsoft.VC*.CRT' | Select-Object -First 1
            if ($taskCrt) { Get-ChildItem -LiteralPath $taskCrt.FullName -Filter '*.dll' | Copy-Item -Destination $taskPackagePath -Force }
        }
        foreach ($taskRuntimeName in @('msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll')) {
            if (-not (Test-Path -LiteralPath (Join-Path $taskPackagePath $taskRuntimeName))) { throw "Missing Visual C++ runtime: $taskRuntimeName. Install the Visual Studio C++ redistributable build component." }
        }
        Copy-Item -LiteralPath (Join-Path $taskProjectRoot 'LICENSE') -Destination (Join-Path $taskPackagePath 'LICENSE-Compositor.txt') -Force
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'README.md') -Destination (Join-Path $taskPackagePath 'README.md') -Force
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'PORTING_STATUS.md') -Destination $taskPackagePath -Force
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'CHANGELOG.md') -Destination $taskPackagePath -Force
        $taskStageReport = Join-Path $PSScriptRoot '阶段报告-0.3.md'
        if (Test-Path -LiteralPath $taskStageReport) { Copy-Item -LiteralPath $taskStageReport -Destination $taskPackagePath -Force }
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'THIRD_PARTY_NOTICES.md') -Destination $taskPackagePath -Force
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'licenses') -Destination $taskPackagePath -Recurse -Force
        Copy-Item -LiteralPath (Join-Path $LibRawRoot 'LICENSE.CDDL') -Destination (Join-Path $taskPackagePath 'licenses\LibRaw-CDDL-1.0.txt') -Force
        Copy-Item -LiteralPath (Join-Path $LibRawRoot 'COPYRIGHT') -Destination (Join-Path $taskPackagePath 'licenses\LibRaw-COPYRIGHT.txt') -Force
        $taskSbom = Join-Path $QtRoot 'sbom'
        if (Test-Path -LiteralPath $taskSbom) { Copy-Item -LiteralPath $taskSbom -Destination $taskPackagePath -Recurse -Force }
        $taskQtVersion = & (Join-Path $QtRoot 'bin\qmake.exe') -query QT_VERSION
        $taskAppVersion = (Select-String -LiteralPath (Join-Path $taskProjectRoot 'CMakeLists.txt') -Pattern 'project\(CompositorWindows VERSION ([^ ]+)').Matches.Groups[1].Value
        "Compositor version: $taskAppVersion`nQt version: $taskQtVersion`nLibRaw version: 0.22.2`nArchitecture: x64`nConfiguration: Release" | Set-Content -LiteralPath (Join-Path $taskPackagePath 'BUILD-INFO.txt') -Encoding utf8
        Compress-Archive -LiteralPath $taskPackagePath -DestinationPath (Join-Path $taskArtifactRoot 'Compositor-Windows-x64.zip') -Force
        Write-Output "Portable application: $taskPackagePath\Compositor.exe"
    }
} finally { $env:PATH = $taskPreviousPath }
