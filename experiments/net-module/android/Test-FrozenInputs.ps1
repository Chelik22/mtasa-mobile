#Requires -Version 7.0
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Frozen-Inputs.ps1')
$base = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'build/input-guard-tests'))
$fixture = Join-Path $base ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path (Join-Path $fixture 'CMakeFiles'), (Join-Path $fixture 'sdk') -Force | Out-Null
$checks = 0
function Require([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message }; $script:checks++ }
function Must-Fail([scriptblock]$Action, [string]$Pattern) {
    try { & $Action | Out-Null } catch { Require ($_.Exception.Message -match $Pattern) "Unexpected rejection: $_"; return }
    throw "Expected rejection matching $Pattern"
}
try {
    $build = Join-Path $fixture 'build.ninja'; $rules = Join-Path $fixture 'CMakeFiles/rules.ninja'
    [IO.File]::WriteAllText($build, "include CMakeFiles/rules.ninja`n")
    $originalRule = "rule compile`n  command = trusted-compiler`n"
    [IO.File]::WriteAllText($rules, $originalRule)
    $expected = Get-NinjaClosureHashes $fixture | ConvertTo-Json | ConvertFrom-Json
    Assert-NinjaClosure -BuildRoot $fixture -Expected $expected
    Require ($expected.PSObject.Properties.Name.Count -eq 2) 'Did not capture full include closure.'
    $mainBefore = (Get-FileHash -LiteralPath $build -Algorithm SHA256).Hash
    [IO.File]::WriteAllText($rules, "rule compile`n  command = different-compiler`n")
    Require ((Get-FileHash -LiteralPath $build -Algorithm SHA256).Hash -eq $mainBefore) 'Test unexpectedly changed build.ninja.'
    Must-Fail { Assert-NinjaClosure -BuildRoot $fixture -Expected $expected } 'Ninja input changed'
    [IO.File]::WriteAllText($rules, $originalRule)
    [IO.File]::WriteAllText((Join-Path $fixture 'sdk/header.h'), '// pinned')
    Assert-ExactIncludeTree -RepositoryRoot $fixture -Roots @('sdk') -ExpectedFiles @('sdk/header.h')
    Require $true 'Original include file set failed.'
    [IO.File]::WriteAllText((Join-Path $fixture 'sdk/vector'), '// nontracked standard-header shadow')
    Must-Fail { Assert-ExactIncludeTree -RepositoryRoot $fixture -Roots @('sdk') -ExpectedFiles @('sdk/header.h') } 'nontracked/ignored'
    Remove-Item -LiteralPath (Join-Path $fixture 'sdk/vector')
    [IO.File]::WriteAllText((Join-Path $fixture 'sdk/.ignored.hpp'), '// filesystem additions must not be hidden by an ignore policy')
    Must-Fail { Assert-ExactIncludeTree -RepositoryRoot $fixture -Roots @('sdk') -ExpectedFiles @('sdk/header.h') } 'nontracked/ignored'
    Remove-Item -LiteralPath (Join-Path $fixture 'sdk/.ignored.hpp')
    Remove-Item -LiteralPath (Join-Path $fixture 'sdk/header.h')
    Must-Fail { Assert-ExactIncludeTree -RepositoryRoot $fixture -Roots @('sdk') -ExpectedFiles @('sdk/header.h') } 'file set changed'
    Write-Output "Frozen input filesystem checks passed: $checks. No Ninja/compiler/Android executable was invoked."
} finally {
    $resolved = [IO.Path]::GetFullPath($fixture)
    if (-not $resolved.StartsWith($base + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Refusing fixture cleanup outside test root.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
