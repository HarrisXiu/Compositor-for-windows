param(
    [string]$BundleRoot = $PSScriptRoot,
    [int]$AdapterIndex = -1,
    [ValidateRange(0,64)][int]$Threads = 0,
    [switch]$CpuOnly,
    [switch]$SkipCpu
)
$ErrorActionPreference = 'Stop'
if ($Threads -eq 0) { $Threads = [Math]::Max(1,[Math]::Min(12,[Math]::Floor([Environment]::ProcessorCount / 2))) }
$taskRoot = [IO.Path]::GetFullPath($BundleRoot)
$taskBin = Join-Path $taskRoot 'bin'
$taskProbe = Join-Path $taskBin 'compositor_ai_probe.exe'
$taskManifest = Join-Path $taskRoot 'references\references.json'
if (-not (Test-Path -LiteralPath $taskProbe) -or -not (Test-Path -LiteralPath $taskManifest)) {
    throw 'Extract the complete AI1 self-test bundle before running this script.'
}
if ($CpuOnly -and $SkipCpu) { throw 'CpuOnly and SkipCpu cannot be used together.' }
$taskRun = Join-Path $taskRoot ('results-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $taskRun | Out-Null
$taskResults = [Collections.Generic.List[object]]::new()
$taskPreviousPath = $env:PATH
$taskPreviousQpa = $env:QT_QPA_PLATFORM
$taskPreviousPluginPath = $env:QT_PLUGIN_PATH
$taskPreviousScreenshot = $env:COMPOSITOR_AI_DIALOG_SCREENSHOT
$taskTranscript = $false
function Invoke-AiCheck([string]$Name, [string]$Executable, [string[]]$Arguments) {
    Write-Host ("Running " + $Name + ' ...')
    $taskLog = Join-Path $taskRun ($Name + '.log')
    $taskWatch = [Diagnostics.Stopwatch]::StartNew()
    # Some native diagnostics are written to stderr even when a check succeeds.
    $taskOldError = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $Executable @Arguments *>&1 | Out-File -LiteralPath $taskLog -Encoding utf8; $taskExit = $LASTEXITCODE }
    finally { $ErrorActionPreference = $taskOldError }
    $taskWatch.Stop()
    $taskState = if ($taskExit -eq 0) { 'passed' } else { 'failed' }
    $taskResults.Add([pscustomobject]@{ name = $Name; status = $taskState; exit_code = $taskExit; elapsed_ms = $taskWatch.ElapsedMilliseconds; log = [IO.Path]::GetFileName($taskLog) })
    Write-Host ("  " + $taskState + ' (' + [math]::Round($taskWatch.Elapsed.TotalSeconds, 1) + ' s)')
    return $taskExit
}
try {
    Start-Transcript -LiteralPath (Join-Path $taskRun 'self-test.log') | Out-Null
    $taskTranscript = $true
    # Resolve every dependency from the extracted bundle and Windows.
    $env:PATH = $taskBin + ';' + (Join-Path $env:SystemRoot 'System32') + ';' + $env:SystemRoot
    $env:QT_QPA_PLATFORM = 'offscreen'
    $env:QT_PLUGIN_PATH = $taskBin
    $env:COMPOSITOR_AI_DIALOG_SCREENSHOT = Join-Path $taskRun 'models-dialog.png'
    $taskHardware = [ordered]@{ utc = [DateTime]::UtcNow.ToString('o'); os = [Environment]::OSVersion.VersionString; processors = @(); display_drivers = @() }
    try {
        $taskHardware.processors = @(Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors)
        $taskHardware.display_drivers = @(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,DriverDate,PNPDeviceID)
    } catch { $taskHardware.inventory_error = $_.Exception.Message }
    $taskHardware | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $taskRun 'hardware.json') -Encoding utf8
    Write-Host 'Checking bundle SHA256 integrity ...'
    $taskFiles = Get-Content -LiteralPath (Join-Path $taskRoot 'SHA256SUMS.json') -Raw | ConvertFrom-Json
    $taskBoundary = $taskRoot.TrimEnd('\') + '\'
    foreach ($taskFile in $taskFiles) {
        $taskPath = [IO.Path]::GetFullPath((Join-Path $taskRoot $taskFile.path))
        if (-not $taskPath.StartsWith($taskBoundary,[StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe bundle integrity path.' }
        if (-not (Test-Path -LiteralPath $taskPath) -or (Get-FileHash -LiteralPath $taskPath -Algorithm SHA256).Hash -ne $taskFile.sha256) {
            throw ('Bundle file is missing or damaged: ' + $taskFile.path)
        }
    }
    $taskResults.Add([pscustomobject]@{ name = 'bundle-integrity'; status = 'passed'; exit_code = 0 })
    $null = Invoke-AiCheck 'runtime-https' $taskProbe @('--runtime-check')
    foreach ($taskSuite in @('model','ai','core','ui','photoshop','raw','dither','layer')) {
        $taskName = if ($taskSuite -eq 'core') { 'compositor_tests.exe' } else { "compositor_$($taskSuite)_tests.exe" }
        if ($taskSuite -eq 'model') { $env:QT_QPA_PLATFORM = 'windows' }
        $taskQtLog = (Join-Path $taskRun ("tests-" + $taskSuite + '-qtest.txt')) + ',txt'
        $null = Invoke-AiCheck ("tests-" + $taskSuite) (Join-Path $taskBin $taskName) @('-o',$taskQtLog)
        $env:QT_QPA_PLATFORM = 'offscreen'
    }
    $env:QT_QPA_PLATFORM = 'windows'
    $null = Invoke-AiCheck 'application-smoke' (Join-Path $taskBin 'Compositor.exe') @('--smoke-test')
    $env:QT_QPA_PLATFORM = 'offscreen'
    $taskAdapterData = & $taskProbe --list-adapters
    if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate DXGI adapters.' }
    $taskAdapterData | Set-Content -LiteralPath (Join-Path $taskRun 'adapters.json') -Encoding utf8
    # Windows PowerShell 5.1 returns a JSON root array as one pipeline object.
    $taskAdapters = @()
    foreach ($taskAdapter in (ConvertFrom-Json -InputObject ($taskAdapterData -join [Environment]::NewLine))) {
        $taskAdapters += $taskAdapter
    }
    if (-not $SkipCpu) {
        $taskOutput = Join-Path $taskRun 'cpu.json'
        $null = Invoke-AiCheck 'cpu-fidelity' $taskProbe @('--references',$taskManifest,'--provider','cpu','--threads',"$Threads",'--output',$taskOutput)
        $taskOutput = Join-Path $taskRun 'cpu-lite-timing.json'
        $null = Invoke-AiCheck 'cpu-lite-timing' $taskProbe @('--references',$taskManifest,'--provider','cpu','--model','birefnet-lite','--threads',"$Threads",'--no-profile','--repeat','3','--output',$taskOutput)
    }
    $taskDevices = @($taskAdapters | Where-Object { $_.directx12 -and -not $_.software })
    Write-Host ('Hardware DX12 adapters detected: ' + $taskDevices.Count)
    if ($AdapterIndex -ge 0) {
        $taskDevices = @($taskDevices | Where-Object { $_.index -eq $AdapterIndex })
        if (-not $taskDevices.Count -and -not $CpuOnly) { throw 'The selected adapter does not support DirectX 12.' }
    }
    if (-not $taskDevices.Count -and $AdapterIndex -lt 0) {
        $taskOutput = Join-Path $taskRun 'no-dx12-auto-fallback.json'
        $null = Invoke-AiCheck 'no-dx12-auto-fallback' $taskProbe @('--references',$taskManifest,'--provider','prefer','--threads',"$Threads",'--output',$taskOutput)
        $taskFallback = Get-Content -LiteralPath $taskOutput -Raw | ConvertFrom-Json
        foreach ($taskModel in $taskFallback.models) {
            foreach ($taskImage in $taskModel.images) {
                if ($taskImage.encoder.backend -ne 'CPU' -or -not $taskImage.encoder.fallback_reason) {
                    throw 'Automatic no-DX12 fallback did not actually use CPU with a diagnostic reason.'
                }
                foreach ($taskClick in $taskImage.clicks) {
                    if ($taskClick.backend -ne 'CPU' -or -not $taskClick.fallback_reason) {
                        throw 'SAM decoder did not actually fall back to CPU.'
                    }
                }
            }
        }
    }
    if (-not $CpuOnly -and $taskDevices.Count) {
        foreach ($taskDevice in $taskDevices) {
            Write-Host ("GPU " + $taskDevice.index + ': ' + $taskDevice.name)
            $taskOutput = Join-Path $taskRun ("dml-" + $taskDevice.index + '.json')
            $taskExit = Invoke-AiCheck ("dml-fidelity-" + $taskDevice.index) $taskProbe @('--references',$taskManifest,'--provider','dml','--adapter',"$($taskDevice.index)",'--threads',"$Threads",'--output',$taskOutput)
            if ($taskExit -ne 0) {
                # Keep the default failure; this second report is diagnostic evidence only.
                $taskOutput = Join-Path $taskRun ("dml-portable-diagnostic-" + $taskDevice.index + '.json')
                $null = Invoke-AiCheck ("dml-portable-diagnostic-" + $taskDevice.index) $taskProbe @('--references',$taskManifest,'--provider','dml','--adapter',"$($taskDevice.index)",'--disable-metacommands','--output',$taskOutput)
            }
        }
    } else {
        $taskResults.Add([pscustomobject]@{ name = 'gpu-fidelity'; status = 'skipped'; reason = $(if ($CpuOnly) { 'CPU-only run requested.' } else { 'No hardware DirectX 12 adapter detected; GPU acceptance is not claimed.' }) })
    }
} catch {
    $taskResults.Add([pscustomobject]@{ name = 'self-test'; status = 'failed'; error = $_.Exception.Message })
    Write-Host $_.Exception.Message -ForegroundColor Red
} finally {
    $env:PATH = $taskPreviousPath
    $env:QT_QPA_PLATFORM = $taskPreviousQpa
    $env:QT_PLUGIN_PATH = $taskPreviousPluginPath
    $env:COMPOSITOR_AI_DIALOG_SCREENSHOT = $taskPreviousScreenshot
    if ($taskTranscript) { Stop-Transcript | Out-Null }
}
$taskFailed = @($taskResults | Where-Object status -eq 'failed').Count
$taskSummary = [ordered]@{
    status = $(if ($taskFailed) { 'failed' } else { 'passed' })
    scope = 'ONNX numerical fidelity and runtime/packaging regression; not ground-truth segmentation quality or Mac parity.'
    cpu_requested = (-not $SkipCpu)
    gpu_requested = (-not $CpuOnly)
    tests = @($taskResults.ToArray())
}
$taskSummary | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $taskRun 'summary.json') -Encoding utf8
$taskZip = $taskRun + '.zip'
Compress-Archive -LiteralPath $taskRun -DestinationPath $taskZip
Write-Host ''
Write-Host ("Result: " + $taskSummary.status)
Write-Host ('Bring this report archive back: ' + $taskZip)
Write-Host 'The report ZIP contains logs and profiles, never model weights or image reference tensors.'
if ($taskFailed) { exit 1 }
exit 0
