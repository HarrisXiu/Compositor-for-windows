param(
    [string]$CacheRoot = (Join-Path (Split-Path -Parent $PSScriptRoot) '.cache')
)
$ErrorActionPreference = 'Stop'
$taskPackages = @(
    @{ Name = 'microsoft.ml.onnxruntime.directml'; Version = '1.24.4'; Hash = '57e9f11b73437bef7a309496135d4c1f96b1a8e9ddba60013fa27bfc1d788681'; Required = 'runtimes\win-x64\native\onnxruntime.dll' },
    @{ Name = 'microsoft.ai.directml'; Version = '1.15.4'; Hash = '4e7cb7ddce8cf837a7a75dc029209b520ca0101470fcdf275c1f49736a3615b9'; Required = 'bin\x64-win\DirectML.dll' }
)
New-Item -ItemType Directory -Path $CacheRoot -Force | Out-Null
$taskRoots = @()
foreach ($taskPackage in $taskPackages) {
    $taskName = "$($taskPackage.Name).$($taskPackage.Version)"
    $taskArchive = Join-Path $CacheRoot "$taskName.nupkg"
    $taskRoot = Join-Path $CacheRoot $taskName
    if (-not (Test-Path -LiteralPath $taskArchive)) {
        $taskDownload = $taskArchive + '.download'
        Invoke-WebRequest -Uri "https://api.nuget.org/v3-flatcontainer/$($taskPackage.Name)/$($taskPackage.Version)/$taskName.nupkg" -OutFile $taskDownload
        if ((Get-FileHash -LiteralPath $taskDownload -Algorithm SHA256).Hash -ne $taskPackage.Hash) { throw "SDK archive SHA256 mismatch: $taskName" }
        Move-Item -LiteralPath $taskDownload -Destination $taskArchive
    }
    if ((Get-FileHash -LiteralPath $taskArchive -Algorithm SHA256).Hash -ne $taskPackage.Hash) { throw "SDK archive SHA256 mismatch: $taskName" }
    if (-not (Test-Path -LiteralPath $taskRoot)) {
        [System.IO.Compression.ZipFile]::ExtractToDirectory($taskArchive, $taskRoot)
    }
    if (-not (Test-Path -LiteralPath (Join-Path $taskRoot $taskPackage.Required))) {
        throw "SDK cache is incomplete: $taskRoot. Use a new CacheRoot rather than overwriting it."
    }
    $taskRoots += [System.IO.Path]::GetFullPath($taskRoot)
}
[pscustomobject]@{ OnnxRuntimeRoot = $taskRoots[0]; DirectMLRoot = $taskRoots[1]; OnnxRuntimeVersion = '1.24.4'; DirectMLVersion = '1.15.4' }
