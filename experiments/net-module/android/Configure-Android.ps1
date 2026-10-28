#Requires -Version 7.0
param(
    [string]$EnetSource = 'D:/projects/astana/work/mtasa-mobile-net-worktree/experiments/mobile-net/build/_deps/enet-src',
    [string]$NdkRoot = 'C:/Android/ndk/28.2.13676358',
    [string]$OutputRoot = (Join-Path $PSScriptRoot 'build/x86_64-api26'),
    [string]$Cmake = 'C:/Program Files/CMake/bin/cmake.exe',
    [string]$Ninja = 'C:/Android/cmake/3.22.1/bin/ninja.exe'
)
$ErrorActionPreference = 'Stop'
$source = [IO.Path]::GetFullPath($PSScriptRoot)
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
$resultPath = Join-Path $OutputRoot 'configure-evidence.json'
$lock = [IO.File]::Open((Join-Path $OutputRoot 'configure.lock'), [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
if (Test-Path -LiteralPath $resultPath) {
    $previous = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
    if ($previous.pid -and (Get-Process -Id $previous.pid -ErrorAction SilentlyContinue)) {
        $lock.Dispose()
        throw "Prior recorded configure PID$($previous.pid) is active; inspect it instead of starting a duplicate."
    }
}
$result = [ordered]@{ status = 'starting'; configure = 'not_run'; targetBuild = 'not_run'; runtime = 'not_run'; hostTraffic = 'not_run'; downloads = $false; abi = 'x86_64'; api = 26; stl = 'c++_static'; startedUtc = [DateTime]::UtcNow.ToString('o') }
$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resultPath -Encoding utf8
$process = $null; $stdout = $null; $stderr = $null
try {
    $toolchain = Join-Path $NdkRoot 'build/cmake/android.toolchain.cmake'
    foreach ($file in @($Cmake, $Ninja, $toolchain, (Join-Path $EnetSource 'CMakeLists.txt'))) {
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing existing tool/cache: $file" }
    }
    $module = [IO.Path]::GetFullPath((Join-Path $source '..'))
    $repo = [IO.Path]::GetFullPath((Join-Path $module '../..'))
    $result.sourceHead = (& git -C $repo rev-parse HEAD | Out-String).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Could not resolve source HEAD.' }
    $result.sourceHashes = [ordered]@{}
    foreach ($relative in @('MtaBitStream.cpp','MtaBitStream.h','ModuleRuntime.cpp','BitBuffer.h','Protocol.h','headless/main.cpp','headless/PacketCodec.h','headless/SourceConstants.h','headless/codec_tests.cpp','protocol_tests.cpp')) {
        $result.sourceHashes[$relative] = (Get-FileHash -LiteralPath (Join-Path $module $relative) -Algorithm SHA256).Hash
    }
    $result.cmakeSha256 = (Get-FileHash -LiteralPath (Join-Path $source 'CMakeLists.txt') -Algorithm SHA256).Hash
    $result.enetManifestSha256 = (Get-FileHash -LiteralPath (Join-Path $repo 'experiments/mobile-net/android/verification/dependency.json') -Algorithm SHA256).Hash
    $result.enetSource = [IO.Path]::GetFullPath($EnetSource)
    $result.ndkRoot = [IO.Path]::GetFullPath($NdkRoot)
    $arguments = @('-S', $source, '-B', $OutputRoot, '-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$Ninja", "-DCMAKE_TOOLCHAIN_FILE=$toolchain", '-DANDROID_ABI=x86_64', '-DANDROID_PLATFORM=android-26', '-DANDROID_STL=c++_static', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', "-DMTA_ENET_SOURCE=$EnetSource")
    $info = [Diagnostics.ProcessStartInfo]::new($Cmake)
    $info.UseShellExecute = $false; $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
    $info.Environment['CMAKE_BUILD_PARALLEL_LEVEL'] = '1'
    foreach ($argument in $arguments) { $info.ArgumentList.Add($argument) }
    $result.arguments = $arguments
    $stdout = [IO.File]::Open((Join-Path $OutputRoot 'configure.stdout.log'), [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    $stderr = [IO.File]::Open((Join-Path $OutputRoot 'configure.stderr.log'), [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    $process = [Diagnostics.Process]::Start($info)
    $result.pid = $process.Id; $result.processStartedUtc = $process.StartTime.ToUniversalTime().ToString('o'); $result.status = 'running'
    $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resultPath -Encoding utf8
    $outTask = $process.StandardOutput.BaseStream.CopyToAsync($stdout); $errTask = $process.StandardError.BaseStream.CopyToAsync($stderr)
    # Configure can include tiny compiler capability probes, but never builds the targets.
    $process.WaitForExit()
    [Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]@($outTask, $errTask))
    $result.exitCode = $process.ExitCode
    if ($process.ExitCode -ne 0) { throw "CMake configure failed with exit$($process.ExitCode); inspect configure logs." }
    if (Test-Path -LiteralPath (Join-Path $OutputRoot 'CTestTestfile.cmake')) { throw 'Android configure unexpectedly registered runtime CTest execution.' }
    $result.configure = 'passed'; $result.status = 'configured_not_built'
} catch { $result.status = 'failed'; $result.error = $_.Exception.Message; throw }
finally {
    if ($process -and -not $process.HasExited) { $result.status = 'running_after_observer_exit' }
    $result.finishedUtc = [DateTime]::UtcNow.ToString('o')
    $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resultPath -Encoding utf8
    if ($stdout) { $stdout.Dispose() }; if ($stderr) { $stderr.Dispose() }
    if ($lock) { $lock.Dispose() }
}
Write-Output $resultPath
