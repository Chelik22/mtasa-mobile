#Requires -Version 7.0
param(
    [switch]$Run,
    [string]$BuildRoot = (Join-Path $PSScriptRoot 'build/x86_64-api26'),
    [string]$NdkRoot = 'C:/Android/ndk/28.2.13676358',
    [string]$Ninja = 'C:/Android/cmake/3.22.1/bin/ninja.exe',
    [ValidateRange(2048, 16384)][int]$RequiredFreeMemoryMiB = 2048
)
$ErrorActionPreference = 'Stop'
$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
$source = [IO.Path]::GetFullPath($PSScriptRoot)
$module = [IO.Path]::GetFullPath((Join-Path $source '..'))
$repo = [IO.Path]::GetFullPath((Join-Path $module '../..'))
if (-not (Test-Path -LiteralPath (Join-Path $BuildRoot 'build.ninja'))) { throw 'Existing reviewed configure cache required; this runner never configures.' }
$lock = [IO.File]::Open((Join-Path $BuildRoot 'build.lock'), [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
$activePath = Join-Path $BuildRoot 'build-active.json'
$attempt = $null; $statePath = $null; $canWrite = $false
$state = [ordered]@{ status = 'preflight'; build = 'not_run'; elf = 'not_run'; runtime = 'not_run'; hostTraffic = 'not_run'; downloads = $false; workers = 1; steps = @(); activeProcess = $null; artifacts = @() }
function Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function Utc([object]$Value) {
    if ($Value -is [DateTime]) { return $Value.ToUniversalTime() }
    if ($Value -is [DateTimeOffset]) { return $Value.UtcDateTime }
    [DateTime]::Parse([string]$Value, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::RoundtripKind).ToUniversalTime()
}
function Save-State {
    if ($canWrite) {
        $state | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $statePath -Encoding utf8
        $state | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $activePath -Encoding utf8
    }
}
function Verify-Inputs {
    foreach ($entry in $configured.sourceHashes.PSObject.Properties) {
        if ((Hash (Join-Path $module $entry.Name)) -ne $entry.Value) { throw "Application source differs from reviewed configure: $($entry.Name)" }
    }
    if ((Hash (Join-Path $source 'CMakeLists.txt')) -ne $configured.cmakeSha256) { throw 'Android CMake source changed since configure.' }
    foreach ($entry in @{ 'compile_commands.json' = $snapshot.compileCommandsSha256; 'build.ninja' = $snapshot.buildNinjaSha256 }.GetEnumerator()) {
        if ((Hash (Join-Path $BuildRoot $entry.Key)) -ne $entry.Value) { throw "Frozen generated input changed: $($entry.Key)" }
    }
    if ($state.cacheSha256 -and (Hash (Join-Path $BuildRoot 'CMakeCache.txt')) -ne $state.cacheSha256) { throw 'CMake cache changed during build.' }
    if ((Hash $dependencyPath) -ne $configured.enetManifestSha256) { throw 'Pinned dependency manifest changed.' }
    $entries = @(Get-ChildItem -LiteralPath $configured.enetSource -Recurse -Force)
    if ((Get-Item -LiteralPath $configured.enetSource).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'ENet root is a reparse point.' }
    if (@($entries | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw 'ENet cache contains symlinks/reparse points.' }
    $actualFiles = @($entries | Where-Object { -not $_.PSIsContainer } | ForEach-Object { [IO.Path]::GetRelativePath($configured.enetSource, $_.FullName).Replace('\', '/') })
    $expectedFiles = @($dependency.files.PSObject.Properties.Name)
    if ($expectedFiles.Count -ne 38 -or @(Compare-Object -ReferenceObject $expectedFiles -DifferenceObject $actualFiles -CaseSensitive).Count) { throw 'ENet exact file set changed.' }
    foreach ($entry in $dependency.files.PSObject.Properties) {
        if ((Hash (Join-Path $configured.enetSource $entry.Name)) -ne $entry.Value.ToUpperInvariant()) { throw "ENet source changed: $($entry.Name)" }
    }
    $changed = @(& git -C $repo diff --name-only $configured.sourceHead -- Shared/sdk Server/sdk experiments/net-module ':!experiments/net-module/android')
    if ($LASTEXITCODE -ne 0 -or $changed.Count) { throw 'Tracked protocol/SDK inputs differ from frozen configure source.' }
}
function Invoke-OwnedTool([string]$Name, [string]$Executable, [string[]]$Arguments) {
    $record = [ordered]@{ name = $Name; executable = [IO.Path]::GetFullPath($Executable); arguments = $Arguments; pid = $null; exitCode = $null; status = 'starting' }
    $state.steps += $record
    $stdout = $null; $stderr = $null; $process = $null; $outTask = $null; $errTask = $null
    try {
        $info = [Diagnostics.ProcessStartInfo]::new($Executable)
        $info.UseShellExecute = $false; $info.CreateNoWindow = $true
        $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
        $info.Environment['CMAKE_BUILD_PARALLEL_LEVEL'] = '1'
        foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
        $record.stdout = "$Name.stdout.log"; $record.stderr = "$Name.stderr.log"
        $stdout = [IO.File]::Open((Join-Path $attempt $record.stdout), [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
        $stderr = [IO.File]::Open((Join-Path $attempt $record.stderr), [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
        $process = [Diagnostics.Process]::Start($info)
        $outTask = $process.StandardOutput.BaseStream.CopyToAsync($stdout); $errTask = $process.StandardError.BaseStream.CopyToAsync($stderr)
        $record.pid = $process.Id; $record.startedUtc = $process.StartTime.ToUniversalTime().ToString('o'); $record.status = 'running'
        $state.activeProcess = @{ pid = $process.Id; executable = $record.executable; startedUtc = $record.startedUtc }
        Write-Output "$Name started PID=$($process.Id), logs=$attempt"
        Save-State
        try { if (-not $process.HasExited) { $process.PriorityClass = [Diagnostics.ProcessPriorityClass]::BelowNormal } }
        catch { $record.priorityError = $_.Exception.Message }
        while (-not $process.WaitForExit(5000)) { Save-State }
        [Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]@($outTask, $errTask))
        $record.exitCode = $process.ExitCode
        if ($process.ExitCode -ne 0) { throw "$Name failed with exit $($process.ExitCode)." }
        $record.status = 'passed'
    } catch { $record.status = 'failed'; $record.error = $_.Exception.Message; throw }
    finally {
        if ($process -and -not $process.HasExited) {
            try { $process.Kill($true) } catch { $record.cleanupError = $_.Exception.Message }
            # Keep drains and lock alive if own tree cannot stop immediately.
            $process.WaitForExit()
        }
        if ($outTask -and $errTask) {
            try { [Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]@($outTask, $errTask)) }
            catch { $record.outputDrainError = $_.Exception.Message; $record.status = 'failed' }
        }
        if ($process) { $record.exitCode = $process.ExitCode }
        $record.finishedUtc = [DateTime]::UtcNow.ToString('o'); $state.activeProcess = $null
        if ($stdout) { $stdout.Dispose() }; if ($stderr) { $stderr.Dispose() }
        if ($record.stdout -and (Test-Path -LiteralPath (Join-Path $attempt $record.stdout))) { $record.stdoutSha256 = Hash (Join-Path $attempt $record.stdout) }
        if ($record.stderr -and (Test-Path -LiteralPath (Join-Path $attempt $record.stderr))) { $record.stderrSha256 = Hash (Join-Path $attempt $record.stderr) }
        if ($record.pid) { [IO.File]::WriteAllText((Join-Path $attempt "$Name.exit.txt"), [string]$record.exitCode) }
        Save-State
    }
    if ($record.status -ne 'passed') { throw "$Name output capture failed." }
}
try {
    if (Test-Path -LiteralPath $activePath) {
        $previous = Get-Content -LiteralPath $activePath -Raw | ConvertFrom-Json
        if (-not $previous.status) { throw 'Invalid previous build receipt; preserved for inspection.' }
        if ($previous.activeProcess) {
            $prior = Get-Process -Id $previous.activeProcess.pid -ErrorAction SilentlyContinue
            if ($prior -and (-not $prior.Path -or -not $previous.activeProcess.executable -or -not $previous.activeProcess.startedUtc)) { throw 'Prior live step identity cannot be verified; inspect it first.' }
            if ($prior -and [string]::Equals($prior.Path, $previous.activeProcess.executable, [StringComparison]::OrdinalIgnoreCase) -and [Math]::Abs(($prior.StartTime.ToUniversalTime() - (Utc $previous.activeProcess.startedUtc)).TotalSeconds) -le 1) { throw 'Prior owned step remains active; follow that PID instead of starting a duplicate.' }
        }
    }
    $attempt = Join-Path $BuildRoot ('runs/' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffffffZ'))
    New-Item -ItemType Directory -Path $attempt -Force | Out-Null
    $statePath = Join-Path $attempt 'result.json'; $canWrite = $true
    $state.startedUtc = [DateTime]::UtcNow.ToString('o'); $state.attemptRoot = $attempt
    $driver = Get-Process -Id $PID
    $state.driverPid = $PID; $state.driverStartedUtc = $driver.StartTime.ToUniversalTime().ToString('o')
    Save-State
    $snapshot = Get-Content -LiteralPath (Join-Path $source 'verification/configure-2026-10-28.json') -Raw | ConvertFrom-Json
    $configured = $snapshot.actualConfigure
    if ($configured.configure -ne 'passed' -or $configured.exitCode -ne 0) { throw 'Reviewed configure snapshot is not passed.' }
    $dependencyPath = Join-Path $repo 'experiments/mobile-net/android/verification/dependency.json'
    $dependency = Get-Content -LiteralPath $dependencyPath -Raw | ConvertFrom-Json
    $readelf = Join-Path $NdkRoot 'toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe'
    foreach ($file in @($Ninja, $readelf)) { if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing installed tool: $file" } }
    $cache = @{}
    foreach ($line in Get-Content -LiteralPath (Join-Path $BuildRoot 'CMakeCache.txt')) { if ($line -match '^([^#/:=]+):[^=]+=(.*)$') { $cache[$Matches[1]] = $Matches[2] } }
    if ($cache.ANDROID_ABI -ne 'x86_64' -or $cache.ANDROID_PLATFORM -ne 'android-26' -or $cache.ANDROID_STL -ne 'c++_static' -or $cache.CMAKE_BUILD_TYPE -ne 'Release') { throw 'Cache target profile changed.' }
    foreach ($pair in @(@($cache.CMAKE_HOME_DIRECTORY, $source), @($cache.CMAKE_MAKE_PROGRAM, $Ninja), @($cache.CMAKE_TOOLCHAIN_FILE, (Join-Path $NdkRoot 'build/cmake/android.toolchain.cmake')))) { if (-not [string]::Equals([IO.Path]::GetFullPath($pair[0]), [IO.Path]::GetFullPath($pair[1]), [StringComparison]::OrdinalIgnoreCase)) { throw 'Cache source/tool path changed; do not copy or silently reconfigure.' } }
    Verify-Inputs
    $state.sourceHead = (& git -C $repo rev-parse HEAD | Out-String).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Could not resolve source HEAD.' }
    $state.configuredSourceHead = $configured.sourceHead; $state.sourceHashes = $configured.sourceHashes
    $state.cmakeSha256 = $configured.cmakeSha256; $state.dependencyManifestSha256 = Hash $dependencyPath
    $state.enetCommit = $dependency.enetCommit; $state.enetArchiveSha256 = $dependency.archiveSha256; $state.verifiedEnetFiles = 38
    $state.graphSha256 = Hash (Join-Path $BuildRoot 'build.ninja'); $state.runnerSha256 = Hash $PSCommandPath
    $state.cacheSha256 = Hash (Join-Path $BuildRoot 'CMakeCache.txt')
    $state.toolchainSha256 = Hash (Join-Path $NdkRoot 'build/cmake/android.toolchain.cmake')
    $state.ndkSourcePropertiesSha256 = Hash (Join-Path $NdkRoot 'source.properties')
    $state.ninjaSha256 = Hash $Ninja; $state.readelfSha256 = Hash $readelf
    $state.freeMemoryMiB = [Math]::Floor((Get-CimInstance Win32_OperatingSystem -OperationTimeoutSec 3).FreePhysicalMemory / 1024)
    $state.requiredFreeMemoryMiB = $RequiredFreeMemoryMiB
    if (-not $Run) { $state.status = 'plan_only'; Save-State; Write-Output $statePath; return }
    if ($state.freeMemoryMiB -lt $RequiredFreeMemoryMiB) { $state.status = 'deferred_memory'; Save-State; Write-Output $statePath; return }
    $targets = @('headless_client', 'headless_codec_tests', 'protocol_tests')
    $before = @{}
    foreach ($target in $targets) { $path = Join-Path $BuildRoot $target; $before[$target] = if (Test-Path -LiteralPath $path) { @{ sha256 = Hash $path; modifiedUtc = (Get-Item -LiteralPath $path).LastWriteTimeUtc } } else { $null } }
    Invoke-OwnedTool 'plan' $Ninja (@('-C', $BuildRoot, '-j', '1', '-n') + $targets)
    if ([IO.File]::ReadAllText((Join-Path $attempt 'plan.stdout.log')) -match 'Re-running CMake|RERUN_CMAKE') { throw 'Ninja requests reconfigure; frozen cache must be inspected first.' }
    $state.freeMemoryAfterPlanMiB = [Math]::Floor((Get-CimInstance Win32_OperatingSystem -OperationTimeoutSec 3).FreePhysicalMemory / 1024)
    if ($state.freeMemoryAfterPlanMiB -lt $RequiredFreeMemoryMiB) { $state.status = 'deferred_memory_after_plan'; Save-State; Write-Output $statePath; return }
    $buildStarted = [DateTime]::UtcNow
    $state.buildStartedUtc = $buildStarted.ToString('o'); $state.status = 'building'; $state.build = 'in_progress'; Save-State
    Invoke-OwnedTool 'build' $Ninja (@('-C', $BuildRoot, '-j', '1') + $targets)
    Verify-Inputs
    $state.elf = 'in_progress'
    foreach ($target in $targets) {
        $path = Join-Path $BuildRoot $target; $file = Get-Item -LiteralPath $path
        $fresh = $file.LastWriteTimeUtc -ge $buildStarted -and (-not $before[$target] -or $file.LastWriteTimeUtc -ne $before[$target].modifiedUtc)
        $artifact = [ordered]@{ name = $target; sha256 = Hash $path; bytes = $file.Length; modifiedUtc = $file.LastWriteTimeUtc.ToString('o'); freshlyProduced = $fresh; runtime = 'not_run' }
        $state.artifacts += $artifact
        Invoke-OwnedTool "elf-$target" $readelf @('-h', '-l', '-d', $path)
        $elf = [IO.File]::ReadAllText((Join-Path $attempt "elf-$target.stdout.log"))
        if ($elf -notmatch 'Class:\s+ELF64' -or $elf -notmatch 'Type:\s+DYN' -or $elf -notmatch 'Machine:\s+Advanced Micro Devices X86-64' -or $elf -notmatch '/system/bin/linker64' -or $elf -match 'libc\+\+_shared\.so|libstdc\+\+\.so') { throw "Unexpected Android ELF/STL for $target." }
        $artifact.elf = 'passed'
    }
    $state.elf = 'passed'
    if (@($state.artifacts | Where-Object { -not $_.freshlyProduced }).Count) { $state.status = 'reused_artifacts_not_new_build'; $state.build = 'not_new_build' }
    else { $state.status = 'built_fresh'; $state.build = 'passed' }
} catch {
    $state.status = 'failed'; $state.error = $_.Exception.Message
    if ($state.build -eq 'in_progress') { $state.build = 'failed' }
    if ($state.elf -eq 'in_progress') { $state.elf = 'failed' }
    throw
}
finally {
    $state.finishedUtc = [DateTime]::UtcNow.ToString('o')
    try { Save-State } finally { $lock.Dispose() }
}
Write-Output $statePath
if ($state.build -eq 'not_new_build') { exit 2 }
