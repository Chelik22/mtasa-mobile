function Get-NinjaClosureHashes {
    param([string]$BuildRoot)
    $root = [IO.Path]::GetFullPath($BuildRoot)
    $pending = [Collections.Generic.Stack[string]]::new(); $pending.Push('build.ninja')
    $hashes = [ordered]@{}
    while ($pending.Count) {
        $relative = $pending.Pop()
        if ($hashes.Contains($relative)) { continue }
        if ($relative -match '[$#]') { throw 'Variable/escaped Ninja include path requires explicit review.' }
        $path = [IO.Path]::GetFullPath((Join-Path $root $relative))
        if (-not $path.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Ninja include escaped frozen build root.' }
        $check = $path
        while (-not [string]::Equals($check, $root, [StringComparison]::OrdinalIgnoreCase)) {
            if ((Get-Item -LiteralPath $check).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Ninja input is reached through a reparse point.' }
            $check = [IO.Path]::GetDirectoryName($check)
        }
        $hashes[$relative] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        $text = [IO.File]::ReadAllText($path)
        foreach ($include in [regex]::Matches($text, '(?m)^\s*(?:include|subninja)\s+([^\r\n]+)')) {
            $pending.Push($include.Groups[1].Value.Trim().Replace('\', '/'))
        }
    }
    return $hashes
}

function Assert-NinjaClosure {
    param([string]$BuildRoot, [object]$Expected)
    if (-not $Expected) { throw 'Reviewed Ninja include closure hashes required.' }
    $actual = Get-NinjaClosureHashes $BuildRoot
    $expectedNames = @($Expected.PSObject.Properties.Name)
    if ($expectedNames.Count -ne $actual.Count -or @(Compare-Object $expectedNames @($actual.Keys) -CaseSensitive).Count) { throw 'Ninja input closure file set changed.' }
    foreach ($entry in $Expected.PSObject.Properties) {
        if ($actual[$entry.Name] -ne $entry.Value.ToUpperInvariant()) { throw "Ninja input changed: $($entry.Name)" }
    }
}

function Get-IncludeTreeFiles {
    param([string]$RepositoryRoot, [string[]]$Roots, [string[]]$ExcludePrefixes = @())
    $repo = [IO.Path]::GetFullPath($RepositoryRoot)
    $pending = [Collections.Generic.Stack[string]]::new()
    foreach ($root in $Roots) { $pending.Push((Join-Path $repo $root)) }
    while ($pending.Count) {
        $directory = $pending.Pop()
        if ((Get-Item -LiteralPath $directory).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Include root contains a reparse point.' }
        foreach ($entry in Get-ChildItem -LiteralPath $directory -Force) {
            $relative = [IO.Path]::GetRelativePath($repo, $entry.FullName).Replace('\', '/')
            $excluded = $false
            foreach ($prefix in $ExcludePrefixes) { if ($relative -eq $prefix -or $relative.StartsWith($prefix + '/', [StringComparison]::OrdinalIgnoreCase)) { $excluded = $true; break } }
            if ($excluded) { continue }
            if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Include input is a reparse point: $relative" }
            if ($entry.PSIsContainer) { $pending.Push($entry.FullName) } else { Write-Output $relative }
        }
    }
}

function Assert-ExactIncludeTree {
    param([string]$RepositoryRoot, [string[]]$Roots, [string[]]$ExpectedFiles, [string[]]$ExcludePrefixes = @())
    $actual = @(Get-IncludeTreeFiles -RepositoryRoot $RepositoryRoot -Roots $Roots -ExcludePrefixes $ExcludePrefixes)
    if ($actual.Count -ne $ExpectedFiles.Count -or ($actual.Count -gt 0 -and @(Compare-Object $ExpectedFiles $actual -CaseSensitive).Count)) { throw 'Include input file set changed, including nontracked/ignored additions.' }
}
