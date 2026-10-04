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
$taskAiSdk = & (Join-Path $PSScriptRoot 'fetch-onnx-runtime.ps1')
& $taskCMake -S $taskProjectRoot -B $taskBuildRoot -A x64 "-DCMAKE_PREFIX_PATH=$QtRoot" "-DCOMPOSITOR_LIBRAW_ROOT=$LibRawRoot" "-DCOMPOSITOR_ONNX_ROOT=$($taskAiSdk.OnnxRuntimeRoot)" "-DCOMPOSITOR_DML_ROOT=$($taskAiSdk.DirectMLRoot)" -DBUILD_TESTING=ON
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
        $taskPackagePath = [System.IO.Path]::GetFullPath($taskPackagePath)
        $taskArtifactBoundary = [System.IO.Path]::GetFullPath($taskArtifactRoot).TrimEnd('\') + '\'
        if (-not $taskPackagePath.StartsWith($taskArtifactBoundary, [System.StringComparison]::OrdinalIgnoreCase)) { throw 'Portable package path must stay inside artifacts.' }
        # Start clean so files from an earlier deployment can't linger in the package.
        if (Test-Path -LiteralPath $taskPackagePath) { Remove-Item -LiteralPath $taskPackagePath -Recurse -Force }
        New-Item -ItemType Directory -Path $taskPackagePath -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $taskBuildRoot "$Configuration\Compositor.exe") -Destination $taskPackagePath -Force
        Copy-Item -LiteralPath (Join-Path $taskBuildRoot "$Configuration\compositor_ai_probe.exe") -Destination $taskPackagePath -Force
        Copy-Item -LiteralPath (Join-Path $LibRawRoot 'bin\libraw.dll') -Destination $taskPackagePath -Force
        $taskThirdPartySource = Join-Path $taskPackagePath 'third-party-source'
        New-Item -ItemType Directory -Path $taskThirdPartySource -Force | Out-Null
        if (-not (Test-Path -LiteralPath $taskLibRawArchive)) { throw 'Include the matching LibRaw source archive for redistribution.' }
        Copy-Item -LiteralPath $taskLibRawArchive -Destination $taskThirdPartySource -Force
        # Deploy Qt Network and Schannel TLS for optional HTTPS model downloads.
        & (Join-Path $QtRoot 'bin\windeployqt.exe') --release --no-translations --compiler-runtime --no-opengl-sw --skip-plugin-types generic,networkinformation --include-plugins qschannelbackend $taskPackagePath
        if ($LASTEXITCODE -ne 0) { throw 'Qt deployment failed.' }
        # Qt Core imports Windows' own ICU (System32, Windows 10 1703 and later); that copy is an OS file, not ours to redistribute.
        foreach ($taskIcuName in @('icu.dll','icuuc.dll','icuin.dll')) {
            $taskIcu = Join-Path $taskPackagePath $taskIcuName
            if (Test-Path -LiteralPath $taskIcu) { Remove-Item -LiteralPath $taskIcu -Force }
        }
        foreach ($taskAiRuntime in @('onnxruntime.dll','onnxruntime_providers_shared.dll','DirectML.dll')) {
            Copy-Item -LiteralPath (Join-Path $taskBuildRoot "$Configuration\$taskAiRuntime") -Destination $taskPackagePath -Force
        }
        foreach ($taskNetworkFile in @('Qt6Network.dll','tls\qschannelbackend.dll')) {
            if (-not (Test-Path -LiteralPath (Join-Path $taskPackagePath $taskNetworkFile))) { throw "Missing HTTPS runtime: $taskNetworkFile" }
        }
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
        foreach ($taskAiDocument in @('AI1_REPORT.md','AI1_SELF_TEST.md')) {
            Copy-Item -LiteralPath (Join-Path $PSScriptRoot $taskAiDocument) -Destination $taskPackagePath -Force
        }
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'PORTING_STATUS.md') -Destination $taskPackagePath -Force
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'CHANGELOG.md') -Destination $taskPackagePath -Force
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'ARCHITECTURE.md') -Destination $taskPackagePath -Force
        $taskCurrentReport = Join-Path $PSScriptRoot '阶段报告-0.4.md'
        if (Test-Path -LiteralPath $taskCurrentReport) { Copy-Item -LiteralPath $taskCurrentReport -Destination $taskPackagePath -Force }
        $taskStageReport = Join-Path $PSScriptRoot '阶段报告-0.3.md'
        if (Test-Path -LiteralPath $taskStageReport) { Copy-Item -LiteralPath $taskStageReport -Destination $taskPackagePath -Force }
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'THIRD_PARTY_NOTICES.md') -Destination $taskPackagePath -Force
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'licenses') -Destination $taskPackagePath -Recurse -Force
        Copy-Item -LiteralPath (Join-Path $LibRawRoot 'LICENSE.CDDL') -Destination (Join-Path $taskPackagePath 'licenses\LibRaw-CDDL-1.0.txt') -Force
        Copy-Item -LiteralPath (Join-Path $LibRawRoot 'COPYRIGHT') -Destination (Join-Path $taskPackagePath 'licenses\LibRaw-COPYRIGHT.txt') -Force
        foreach ($taskLicense in @(
            @{ Root = $taskAiSdk.OnnxRuntimeRoot; File = 'LICENSE'; Name = 'ONNX-Runtime-MIT.txt' },
            @{ Root = $taskAiSdk.OnnxRuntimeRoot; File = 'ThirdPartyNotices.txt'; Name = 'ONNX-Runtime-ThirdPartyNotices.txt' },
            @{ Root = $taskAiSdk.DirectMLRoot; File = 'LICENSE.txt'; Name = 'DirectML-LICENSE.txt' },
            @{ Root = $taskAiSdk.DirectMLRoot; File = 'LICENSE-CODE.txt'; Name = 'DirectML-CODE-LICENSE.txt' },
            @{ Root = $taskAiSdk.DirectMLRoot; File = 'ThirdPartyNotices.txt'; Name = 'DirectML-ThirdPartyNotices.txt' }
        )) {
            Copy-Item -LiteralPath (Join-Path $taskLicense.Root $taskLicense.File) -Destination (Join-Path $taskPackagePath "licenses\$($taskLicense.Name)") -Force
        }
        $taskSbom = Join-Path $QtRoot 'sbom'
        if (Test-Path -LiteralPath $taskSbom) { Copy-Item -LiteralPath $taskSbom -Destination $taskPackagePath -Recurse -Force }
        # Qt's source is published beside the release (not inside the ZIP); its license texts go in the package.
        $taskQtSourceCache = & (Join-Path $PSScriptRoot 'fetch-qt-source.ps1') -Destination (Join-Path $taskArtifactRoot 'qt-source')
        foreach ($taskModule in @('qtbase','qtsvg','qtimageformats')) {
            $taskExtract = Join-Path $taskBuildRoot "qt-licenses\$taskModule"
            $taskExtract = [System.IO.Path]::GetFullPath($taskExtract)
            $taskBuildBoundary = [System.IO.Path]::GetFullPath($taskBuildRoot).TrimEnd('\') + '\'
            if (-not $taskExtract.StartsWith($taskBuildBoundary, [System.StringComparison]::OrdinalIgnoreCase)) { throw 'Qt license extraction path must stay inside the build directory.' }
            if (Test-Path -LiteralPath $taskExtract) { Remove-Item -LiteralPath $taskExtract -Recurse -Force }
            New-Item -ItemType Directory -Path $taskExtract -Force | Out-Null
            & (Join-Path $env:SystemRoot 'System32\tar.exe') -xf (Join-Path $taskQtSourceCache "$taskModule-everywhere-src-6.10.2.tar.xz") -C $taskExtract "$taskModule-everywhere-src-6.10.2/LICENSES"
            if ($LASTEXITCODE -ne 0) { throw "Cannot extract $taskModule license texts." }
            Copy-Item -LiteralPath (Join-Path $taskExtract "$taskModule-everywhere-src-6.10.2\LICENSES") -Destination (Join-Path $taskPackagePath "licenses\Qt\$taskModule") -Recurse -Force
        }
        # A readable list of the third-party code inside the deployed Qt modules, taken from Qt's own SBOM.
        # Bootstrap entries are build tools that never ship.
        $taskAttributions = foreach ($taskSpdx in Get-ChildItem -LiteralPath (Join-Path $taskPackagePath 'sbom') -Filter '*.spdx.json') {
            foreach ($taskEntry in (Get-Content -LiteralPath $taskSpdx.FullName -Raw -Encoding utf8 | ConvertFrom-Json).packages) {
                if ($taskEntry.name -notmatch 'Attribution|^Bundled' -or $taskEntry.name -match '^Bootstrap') { continue }
                $taskTitle = if ($taskEntry.comment -match '(?m)^\s*Name: (.+)$') { $Matches[1].Trim() } else { $taskEntry.name }
                $taskUsage = if ($taskEntry.comment -match '(?m)^\s*Qt usage: (.+)$') { $Matches[1].Trim() } else { '' }
                [pscustomobject]@{ Title = $taskTitle; Version = $taskEntry.versionInfo; License = $taskEntry.licenseConcluded; Usage = $taskUsage; Copyright = $taskEntry.copyrightText; Source = $taskEntry.downloadLocation }
            }
        }
        $taskList = @("Third-party components in the deployed Qt 6.10.2 modules (qtbase, qtsvg, qtimageformats).",
            "Generated from Qt's SBOM in sbom/; qtbase entries cover all of its modules, so this list is a superset of what ships.",
            "License texts for each identifier are in licenses/Qt/<module>/. Qt's complete source is published beside each release.", '')
        foreach ($taskItem in $taskAttributions | Sort-Object Title, Version -Unique) {
            $taskList += "$($taskItem.Title) $($taskItem.Version)"
            $taskList += "  License: $($taskItem.License)"
            if ($taskItem.Usage) { $taskList += "  Qt usage: $($taskItem.Usage)" }
            if ($taskItem.Copyright -and $taskItem.Copyright -ne 'NOASSERTION') { $taskList += ($taskItem.Copyright -split "`n" | ForEach-Object { "  $_" }) }
            if ($taskItem.Source -and $taskItem.Source -ne 'NOASSERTION') { $taskList += "  Source: $($taskItem.Source)" }
            $taskList += ''
        }
        $taskList | Set-Content -LiteralPath (Join-Path $taskPackagePath 'licenses\Qt-third-party-components.txt') -Encoding utf8
        $taskQtVersion = & (Join-Path $QtRoot 'bin\qmake.exe') -query QT_VERSION
        $taskAppVersion = (Select-String -LiteralPath (Join-Path $taskProjectRoot 'CMakeLists.txt') -Pattern 'project\(CompositorWindows VERSION ([^ ]+)').Matches.Groups[1].Value
        "Compositor version: $taskAppVersion`nQt version: $taskQtVersion`nLibRaw version: 0.22.2`nONNX Runtime DirectML: 1.24.4`nDirectML: 1.15.4`nArchitecture: x64`nConfiguration: Release" | Set-Content -LiteralPath (Join-Path $taskPackagePath 'BUILD-INFO.txt') -Encoding utf8
        Compress-Archive -LiteralPath $taskPackagePath -DestinationPath (Join-Path $taskArtifactRoot 'Compositor-Windows-x64.zip') -Force
        Write-Output "Portable application: $taskPackagePath\Compositor.exe"
    }
} finally { $env:PATH = $taskPreviousPath }
