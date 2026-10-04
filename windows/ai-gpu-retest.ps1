param(
    [string]$BundleRoot,
    [int]$AdapterIndex = -1,
    [ValidateRange(0,64)][int]$Threads = 0,
    [string[]]$Models = @('birefnet-lite','sam2','mobilesam','birefnet'),
    [switch]$LocateOnly,
    [switch]$NoPrompt
)
$ErrorActionPreference = 'Stop'
if ($Threads -eq 0) { $Threads = [Math]::Max(1,[Math]::Min(12,[Math]::Floor([Environment]::ProcessorCount / 2))) }
function Test-AiBundleDirectory([string]$Directory) {
    foreach ($taskRequired in @('bin\compositor_ai_probe.exe','bin\onnxruntime.dll','references\references.json','SHA256SUMS.json')) {
        if (-not (Test-Path -LiteralPath (Join-Path $Directory $taskRequired) -PathType Leaf)) { return $false }
    }
    return (Test-Path -LiteralPath (Join-Path $Directory 'models') -PathType Container)
}
function Find-AiBundleDirectory([string]$Anchor) {
    if (-not $Anchor) { return $null }
    $taskCandidate = [IO.Path]::GetFullPath($Anchor)
    # ZIP extraction commonly adds another directory with the same name.
    for ($taskDepth = 0; $taskDepth -le 4; $taskDepth++) {
        if (Test-AiBundleDirectory $taskCandidate) { return $taskCandidate }
        $taskCandidate = Join-Path $taskCandidate 'AI1-self-test'
    }
    return $null
}
$taskProbe = Join-Path $PSScriptRoot 'probe\compositor_ai_probe.exe'
if (-not (Test-Path -LiteralPath $taskProbe -PathType Leaf)) {
    throw ("Diagnostic probe is missing: " + $taskProbe + ". Extract the entire new ZIP.")
}
$taskRoot = $null
if ($BundleRoot) {
    $taskRoot = Find-AiBundleDirectory $BundleRoot
} else {
    # Support placement inside the bundle, alongside it, and inside an outer ZIP directory.
    $taskAnchor = $PSScriptRoot
    for ($taskLevel = 0; $taskLevel -lt 3 -and $taskAnchor -and -not $taskRoot; $taskLevel++) {
        $taskRoot = Find-AiBundleDirectory $taskAnchor
        $taskParent = [IO.Directory]::GetParent($taskAnchor)
        $taskAnchor = if ($taskParent) { $taskParent.FullName } else { $null }
    }
}
if (-not $taskRoot -and -not $NoPrompt) {
    Add-Type -AssemblyName System.Windows.Forms
    $taskPicker = New-Object System.Windows.Forms.FolderBrowserDialog
    $taskPicker.Description = '请选择原 AI1-self-test 文件夹（包含 bin、models、references），也可选择外层解压目录。'
    $taskPicker.ShowNewFolderButton = $false
    try {
        if ($taskPicker.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
            $taskRoot = Find-AiBundleDirectory $taskPicker.SelectedPath
        }
    } finally { $taskPicker.Dispose() }
}
if (-not $taskRoot) {
    throw '没有找到完整原包。请重新运行并选择包含 bin、models、references、SHA256SUMS.json 的 AI1-self-test 文件夹。'
}
Write-Host ('Original bundle: ' + $taskRoot)
if ($LocateOnly) {
    Write-Host 'Directory layout check passed. GPU tests have not run.'
    exit 0
}
$taskBin = Join-Path $taskRoot 'bin'
$taskManifest = Join-Path $taskRoot 'references\references.json'
$taskStart = Get-Date
$taskRun = Join-Path $taskRoot ('gpu-retest-' + $taskStart.ToString('yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $taskRun | Out-Null
$taskResults = [Collections.Generic.List[object]]::new()
$taskPreviousPath = $env:PATH
$taskPreviousPluginPath = $env:QT_PLUGIN_PATH
$taskTranscript = $false
function Test-AiIntegrity([string]$Root, [string]$Manifest) {
    $taskBoundary = [IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    foreach ($taskFile in (Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json)) {
        $taskPath = [IO.Path]::GetFullPath((Join-Path $Root $taskFile.path))
        if (-not $taskPath.StartsWith($taskBoundary,[StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe integrity path.' }
        if (-not (Test-Path -LiteralPath $taskPath) -or (Get-FileHash -LiteralPath $taskPath -Algorithm SHA256).Hash -ne $taskFile.sha256) {
            throw ('File is missing or damaged: ' + $taskFile.path)
        }
    }
}
function Invoke-AiProbe([string]$Name, [string[]]$Arguments, [bool]$Diagnostic = $false) {
    Write-Host ("Running " + $Name + ' ...')
    $taskLog = Join-Path $taskRun ($Name + '.log')
    $taskOutput = Join-Path $taskRun ($Name + '.json')
    $taskWatch = [Diagnostics.Stopwatch]::StartNew()
    $taskOldError = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $taskProbe @Arguments --output $taskOutput *>&1 | Out-File -LiteralPath $taskLog -Encoding utf8; $taskExit = $LASTEXITCODE }
    finally { $ErrorActionPreference = $taskOldError }
    $taskWatch.Stop()
    $taskState = if ($taskExit -eq 0) { 'passed' } else { 'failed' }
    $taskResults.Add([pscustomobject]@{name=$Name;status=$taskState;diagnostic=$Diagnostic;exit_code=$taskExit;elapsed_ms=$taskWatch.ElapsedMilliseconds;report=[IO.Path]::GetFileName($taskOutput)})
    Write-Host ("  " + $taskState + ' (' + [math]::Round($taskWatch.Elapsed.TotalSeconds,1) + ' s)')
    return $taskExit
}
try {
    Start-Transcript -LiteralPath (Join-Path $taskRun 'gpu-retest.log') | Out-Null
    $taskTranscript = $true
    $env:PATH = $taskBin + ';' + (Join-Path $env:SystemRoot 'System32') + ';' + $env:SystemRoot
    $env:QT_PLUGIN_PATH = $taskBin
    Write-Host 'Verifying the original bundle and diagnostic tool ...'
    Test-AiIntegrity $taskRoot (Join-Path $taskRoot 'SHA256SUMS.json')
    Test-AiIntegrity $PSScriptRoot (Join-Path $PSScriptRoot 'SHA256SUMS.json')
    # Windows searches System32 before PATH. Stage verified DLLs beside the new probe.
    foreach ($taskDll in (Get-ChildItem -LiteralPath $taskBin -Filter '*.dll' -File)) {
        Copy-Item -LiteralPath $taskDll.FullName -Destination (Split-Path -Parent $taskProbe) -Force
    }
    $taskRuntimeData = & $taskProbe --runtime-check
    if ($LASTEXITCODE -ne 0) { throw 'The staged probe runtime check failed.' }
    $taskRuntimeData | Set-Content -LiteralPath (Join-Path $taskRun 'runtime.json') -Encoding utf8
    $taskRuntime = ConvertFrom-Json -InputObject ($taskRuntimeData -join [Environment]::NewLine)
    if ($taskRuntime.onnxruntime -ne '1.24.4') { throw 'The probe loaded an unexpected ONNX Runtime version.' }
    $taskResults.Add([pscustomobject]@{name='integrity-and-runtime';status='passed';diagnostic=$false})
    $taskHardware = [ordered]@{utc=[DateTime]::UtcNow.ToString('o');os=[Environment]::OSVersion.VersionString;threads=$Threads}
    try {
        $taskHardware.processors = @(Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors)
        $taskHardware.display_drivers = @(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,DriverDate,PNPDeviceID)
    } catch { $taskHardware.inventory_error = $_.Exception.Message }
    $taskHardware | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $taskRun 'hardware.json') -Encoding utf8
    $taskAdapterData = & $taskProbe --list-adapters
    if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate DXGI adapters.' }
    $taskAdapterData | Set-Content -LiteralPath (Join-Path $taskRun 'adapters.json') -Encoding utf8
    $taskAdapters = @()
    foreach ($taskAdapter in (ConvertFrom-Json -InputObject ($taskAdapterData -join [Environment]::NewLine))) { $taskAdapters += $taskAdapter }
    $taskDevices = @($taskAdapters | Where-Object { $_.directml_supported -and ($AdapterIndex -lt 0 -or $_.index -eq $AdapterIndex) })
    if (-not $taskDevices.Count) { throw 'No requested Intel integrated GPU is supported. GPU acceptance cannot be claimed.' }
    $taskDefinitions = (Get-Content -LiteralPath $taskManifest -Raw | ConvertFrom-Json).models
    if (-not $Models.Count) { throw 'Select at least one model.' }
    foreach ($taskId in $Models) {
        if ($taskId -notmatch '^[a-z0-9-]+$' -or -not @($taskDefinitions | Where-Object id -eq $taskId).Count) { throw ('Unknown model: ' + $taskId) }
    }
    foreach ($taskDevice in $taskDevices) {
        Write-Host ("GPU " + $taskDevice.index + ': ' + $taskDevice.name)
        foreach ($taskId in $Models) {
            $taskRepeat = if ($taskId -like 'birefnet*') { '3' } else { '1' }
            $taskCommon = @('--references',$taskManifest,'--adapter',"$($taskDevice.index)",'--threads',"$Threads",'--model',$taskId)
            $taskName = "dml-$($taskDevice.index)-$taskId"
            # A separate process gives each model a new device, even after a GPU hang.
            $taskExit = Invoke-AiProbe $taskName ($taskCommon + @('--provider','dml','--repeat',$taskRepeat))
            if ($taskExit -ne 0) {
                # Diagnostics never replace the failed default result.
                $null = Invoke-AiProbe ($taskName + '-portable') ($taskCommon + @('--provider','dml','--repeat',$taskRepeat,'--disable-metacommands')) $true
                if ($taskId -like 'birefnet*') {
                    $null = Invoke-AiProbe ($taskName + '-reverse') ($taskCommon + @('--provider','dml','--reverse-images')) $true
                    foreach ($taskImage in @('cars.png','groceries.png')) {
                        $null = Invoke-AiProbe ($taskName + '-fresh-' + [IO.Path]::GetFileNameWithoutExtension($taskImage)) ($taskCommon + @('--provider','dml','--image',$taskImage,'--repeat','3')) $true
                    }
                }
                $null = Invoke-AiProbe ($taskName + '-auto-fallback') ($taskCommon + @('--provider','prefer')) $true
            }
        }
    }
} catch {
    $taskResults.Add([pscustomobject]@{name='gpu-retest';status='failed';diagnostic=$false;error=$_.Exception.Message})
    Write-Host $_.Exception.Message -ForegroundColor Red
} finally {
    # Read only GPU recovery events from this test window. Missing permissions/events are recorded.
    try {
        $taskEvents = @(Get-WinEvent -FilterHashtable @{LogName='System';StartTime=$taskStart.AddMinutes(-1)} -ErrorAction Stop |
            Where-Object {$_.ProviderName -match 'Display|nvlddmkm|amdwddmg|amdkmdag|DxgKrnl|WHEA'} |
            Select-Object TimeCreated,Id,LevelDisplayName,ProviderName,Message)
        ConvertTo-Json -InputObject $taskEvents -Depth 5 | Set-Content -LiteralPath (Join-Path $taskRun 'gpu-system-events.json') -Encoding utf8
    } catch { $_.Exception.Message | Set-Content -LiteralPath (Join-Path $taskRun 'gpu-system-events-unavailable.txt') -Encoding utf8 }
    $env:PATH = $taskPreviousPath
    $env:QT_PLUGIN_PATH = $taskPreviousPluginPath
    if ($taskTranscript) { Stop-Transcript | Out-Null }
}
$taskFailed = @($taskResults | Where-Object { $_.status -eq 'failed' -and -not $_.diagnostic }).Count
$taskSummary = [ordered]@{status=$(if ($taskFailed) {'failed'} else {'passed'});scope='GPU numerical fidelity with independent model processes; diagnostic successes do not override default failures.';tests=@($taskResults.ToArray())}
$taskSummary | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $taskRun 'summary.json') -Encoding utf8
$taskZip = $taskRun + '.zip'
Compress-Archive -LiteralPath $taskRun -DestinationPath $taskZip
Write-Host ('Result: ' + $taskSummary.status)
Write-Host ('Bring back: ' + $taskZip)
if ($taskFailed) { exit 1 }
exit 0
