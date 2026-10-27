param(
    [Parameter(Mandatory = $true)][string]$EnetSource,
    [string]$NdkRoot = 'C:/Android/ndk/28.2.13676358',
    [string]$OutputRoot = (Join-Path $PSScriptRoot '../build-android'),
    [ValidateSet('arm64-v8a', 'x86_64')][string[]]$Abis = @('arm64-v8a', 'x86_64'),
    [ValidateRange(26, 35)][int]$ApiLevel = 26,
    [string]$Cmake = 'cmake',
    [string]$Ninja = 'C:/Android/cmake/3.22.1/bin/ninja.exe'
)
$ErrorActionPreference = 'Stop'
$sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
# Invalidate a previous successful result before any prerequisite can fail.
[ordered]@{ status = 'running'; recordedAtUtc = [DateTime]::UtcNow.ToString('o'); runtime = 'not_run' } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputRoot 'build-evidence.json')
$records = @()
trap {
    $failure = $_
    try {
        [ordered]@{ status = 'failed'; recordedAtUtc = [DateTime]::UtcNow.ToString('o'); error = $failure.Exception.Message; runtime = 'not_run'; records = $records } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutputRoot 'build-evidence.json')
    } catch { }
    throw $failure
}
$EnetSource = (Resolve-Path -LiteralPath $EnetSource).Path
$NdkRoot = (Resolve-Path -LiteralPath $NdkRoot).Path
$toolchain = Join-Path $NdkRoot 'build/cmake/android.toolchain.cmake'
$readelf = Join-Path $NdkRoot 'toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe'
$ninjaPath = (Get-Command $Ninja -ErrorAction Stop).Source
foreach ($path in @($toolchain, $readelf, (Join-Path $EnetSource 'include/enet/enet.h'), (Join-Path $EnetSource 'CMakeLists.txt'), (Join-Path $EnetSource 'LICENSE'))) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required cached dependency/tool missing: $path" }
}
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null

function Invoke-LoggedTool([string]$Tool, [string[]]$ToolArguments, [string]$LogPath) {
    $savedPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $Tool @ToolArguments 2>&1 | ForEach-Object { [string]$_ } | Tee-Object -FilePath $LogPath
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $savedPreference }
    if ($code -ne 0) { throw "$Tool failed with exit code $code; log: $LogPath" }
}

function Get-Sha256([string]$Path) {
    # Keep evidence collection independent of PowerShell module autoloading.
    $stream = [IO.File]::OpenRead($Path)
    $hasher = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-', '') }
    finally { $hasher.Dispose(); $stream.Dispose() }
}

$dependency = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'verification/dependency.json') -Raw | ConvertFrom-Json
$verifiedFiles = 0
foreach ($entry in $dependency.files.PSObject.Properties) {
    $file = Join-Path $EnetSource $entry.Name
    if (-not (Test-Path -LiteralPath $file -PathType Leaf) -or (Get-Sha256 $file) -ne $entry.Value) {
        throw "ENet cache differs from pinned archive: $($entry.Name)"
    }
    $verifiedFiles++
}
if ($verifiedFiles -ne $dependency.comparedFiles -or $verifiedFiles -eq 0) { throw 'ENet dependency manifest is incomplete.' }

$sourceHashes = [ordered]@{}
foreach ($name in @('wire.hpp', 'wire_tests.cpp', 'net_probe.cpp', 'CMakeLists.txt')) {
    $sourceHashes[$name] = Get-Sha256 (Join-Path $sourceRoot $name)
}
$head = (& git -C $sourceRoot rev-parse HEAD | Out-String).Trim()
foreach ($abi in $Abis) {
    $buildRoot = Join-Path $OutputRoot $abi
    New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null
    Invoke-LoggedTool $Cmake @('-S', $sourceRoot, '-B', $buildRoot, '-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$ninjaPath", "-DCMAKE_TOOLCHAIN_FILE=$toolchain", "-DANDROID_ABI=$abi", "-DANDROID_PLATFORM=android-$ApiLevel", '-DANDROID_STL=c++_static', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', "-DFETCHCONTENT_SOURCE_DIR_ENET=$EnetSource", '-DFETCHCONTENT_FULLY_DISCONNECTED=ON') (Join-Path $buildRoot 'configure.log')
    Invoke-LoggedTool $Cmake @('--build', $buildRoot, '--parallel', '1', '--target', 'net_probe', 'wire_tests') (Join-Path $buildRoot 'build.log')
    $binaries = @()
    foreach ($name in @('net_probe', 'wire_tests')) {
        $binary = Join-Path $buildRoot $name
        $elfLog = Join-Path $buildRoot "$name.readelf.log"
        Invoke-LoggedTool $readelf @('-h', '-l', '-d', $binary) $elfLog
        $elf = Get-Content -LiteralPath $elfLog -Raw
        $expectedMachine = if ($abi -eq 'arm64-v8a') { 'AArch64' } else { 'Advanced Micro Devices X86-64' }
        if ($elf -notmatch ('Machine:\s+' + [Regex]::Escape($expectedMachine))) { throw "Unexpected ELF architecture for $abi/$name" }
        if ($elf -notmatch '/system/bin/linker64') { throw "Missing Android dynamic linker for $abi/$name" }
        $binaries += [ordered]@{ name = $name; sha256 = (Get-Sha256 $binary); bytes = (Get-Item -LiteralPath $binary).Length; elfMachine = $expectedMachine; runtime = 'not_run' }
    }
    if (Test-Path -LiteralPath (Join-Path $buildRoot 'CTestTestfile.cmake')) { throw 'Android build unexpectedly registered executable runtime tests.' }
    $records += [ordered]@{ abi = $abi; apiLevel = $ApiLevel; build = 'passed'; runtime = 'not_run'; binaries = $binaries; compileCommands = (Join-Path $buildRoot 'compile_commands.json') }
}
[ordered]@{ status = 'passed'; recordedAtUtc = [DateTime]::UtcNow.ToString('o'); sourceHead = $head; sourceHashes = $sourceHashes; enetCommit = $dependency.enetCommit; enetArchiveSha256 = $dependency.archiveSha256; verifiedEnetFiles = $verifiedFiles; enetSource = $EnetSource; ndkRoot = $NdkRoot; runtime = 'not_run'; hostAndroidInteraction = 'not_run'; records = $records } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutputRoot 'build-evidence.json')
Write-Output 'Android cross-build and ELF checks passed. No Android executable or CTest was run.'
