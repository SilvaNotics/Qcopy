param([Parameter(Mandatory)][string]$Exe, [string]$TargetParent, [int]$Runs=5,
    [int]$SmallFiles=100000, [int]$LargeGiB=32, [string]$Csv='build/benchmark.csv',
    [ValidateSet('warm','uncontrolled','cold')][string]$Cache='uncontrolled',
    [string]$BeforeTrialScript, [switch]$IncludeExperimentalIocp,
    [ValidateSet('all','small','mixed','large')][string]$Workload='all', [string[]]$Modes)
$ErrorActionPreference = 'Stop'
$Exe = [IO.Path]::GetFullPath($Exe)
$Csv = [IO.Path]::GetFullPath($Csv)
$bin = [IO.Path]::GetDirectoryName($Exe)
$dstParent = if ($TargetParent) { [IO.Path]::GetFullPath($TargetParent) } else { $bin }
$src = Join-Path $bin ('qcopy-bench-' + [guid]::NewGuid())
$dst = Join-Path $dstParent ('qcopy-bench-' + [guid]::NewGuid())
if ($Cache -eq 'cold' -and -not $BeforeTrialScript) { throw 'Cold trials require a cache-reset preparation script.' }
if ([IO.File]::Exists($Csv)) { throw "Choose a new CSV path: $Csv" }
if ($Runs -lt 1 -or $LargeGiB -lt 1 -or $SmallFiles -lt 1) { throw 'Fixture sizes and run count must be positive' }
$space = 512MB
if ($Workload -in @('all','large')) { $space += 1GB * $LargeGiB }
if ([IO.Path]::GetPathRoot($bin) -eq [IO.Path]::GetPathRoot($dstParent)) { $space *= 2 }
foreach ($parent in @($bin, $dstParent)) {
    if ([IO.DriveInfo]::new([IO.Path]::GetPathRoot($parent)).AvailableFreeSpace -lt $space) { throw "Insufficient space on $parent" }
}
function Remove-Fixture([string]$item, [string]$parent) {
    $full = [IO.Path]::GetFullPath($item)
    if ([IO.Path]::GetDirectoryName($full).TrimEnd('\') -ne $parent.TrimEnd('\')) { throw "Invalid fixture path $full" }
    if ([IO.Directory]::Exists($full)) { [IO.Directory]::Delete($full, $true) }
    elseif ([IO.File]::Exists($full)) { [IO.File]::Delete($full) }
}
function Quote([string]$arg) { '"' + $arg + '"' }
function Write-Blocks([string]$path, [byte[]]$data, [int]$count) {
    $f = [IO.File]::OpenWrite($path)
    try { for ($i=0; $i -lt $count; $i++) { $f.Write($data,0,$data.Length) }; $f.Flush($true) }
    finally { $f.Dispose() }
}
function Files([string]$path) {
    if ([IO.Directory]::Exists($path)) { [IO.Directory]::EnumerateFiles($path,'*','AllDirectories') } else { $path }
}
try {
    [IO.Directory]::CreateDirectory($src) | Out-Null
    [IO.Directory]::CreateDirectory($dst) | Out-Null
    $random = [Random]::new(173)
    $small = New-Object byte[] 512
    $block = New-Object byte[] (4MB)
    $random.NextBytes($small); $random.NextBytes($block)
    $smallRoot = [IO.Directory]::CreateDirectory((Join-Path $src 'small')).FullName
    $mixed = [IO.Directory]::CreateDirectory((Join-Path $src 'mixed')).FullName
    if ($Workload -in @('all', 'small')) {
        for ($n=0; $n -lt $SmallFiles; $n++) {
            if (($n % 1000) -eq 0) { $shard = [IO.Directory]::CreateDirectory((Join-Path $smallRoot "s$n")).FullName }
            [IO.File]::WriteAllBytes((Join-Path $shard "$n.bin"), $small)
        }
    }
    if ($Workload -in @('all', 'mixed')) {
        foreach ($n in 0..15) {
            $shard = [IO.Directory]::CreateDirectory((Join-Path $mixed "d$n\nested")).FullName
            foreach ($k in 0..63) { [IO.File]::WriteAllBytes((Join-Path $shard "$k.bin"), $small) }
            Write-Blocks (Join-Path $shard 'medium.bin') $block 2
        }
    }
    if ($Workload -in @('all','large')) { Write-Blocks (Join-Path $src 'large.bin') $block (256*$LargeGiB) }
    $scenarios = @(
        @{ name='small'; item='small'; count=$SmallFiles; bytes=512L*$SmallFiles },
        @{ name='mixed'; item='mixed'; count=1040; bytes=16L*(8MB+64*512) },
        @{ name='large'; item='large.bin'; count=1; bytes=1GB*$LargeGiB })
    foreach ($scenario in $scenarios) {
        if ($Workload -ne 'all' -and $scenario.name -ne $Workload) { continue }
        $trialModes = @('qcopy-auto','copyfile2-single','windows-shell','robocopy-MT','robocopy-J')
        if ($scenario.name -eq 'large') { $trialModes += 'copyfile2-unbuffered' }
        if ($scenario.name -eq 'large' -and $IncludeExperimentalIocp) { $trialModes += 'experimental-iocp' }
        if ($Modes) { $trialModes = @($trialModes | Where-Object { $Modes -contains $_ }) }
        if (-not $trialModes.Count) { throw "No matching modes for $($scenario.name)" }
        $inputPath = Join-Path $src $scenario.item
        $out = Join-Path $dst $scenario.item
        for ($run=1; $run -le $Runs; $run++) {
            foreach ($offset in 0..($trialModes.Count-1)) {
                $mode = $trialModes[($offset+$run-1) % $trialModes.Count]
                $program = $Exe
                $cli = @('copy','--to',(Quote $dst),(Quote $inputPath))
                switch ($mode) {
                    'windows-shell' { $program = Join-Path $bin 'qcopy_shell_bench.exe'; $cli = @((Quote $inputPath),(Quote $dst)) }
                    { $_.StartsWith('robocopy-') } {
                        $program = (Get-Command robocopy.exe).Source
                        if ($scenario.name -eq 'large') { $cli = @((Quote $src),(Quote $dst),'large.bin') }
                        else { $cli = @((Quote $inputPath),(Quote $out),'/E') }
                        $arg = if ($mode -eq 'robocopy-MT') { '/MT:4' } else { '/J' }
                        $cli += @($arg,'/COPY:DAT','/DCOPY:DAT','/R:0','/W:0','/NP','/NFL','/NDL','/NJH','/NJS')
                    }
                    'copyfile2-single' { $cli += @('--benchmark-strategy','native-single') }
                    'copyfile2-unbuffered' { $cli += @('--benchmark-strategy','native-unbuffered') }
                    'experimental-iocp' { $cli += @('--benchmark-strategy','experimental-iocp') }
                }
                if ($BeforeTrialScript) { & $BeforeTrialScript $inputPath $dst }
                if ($Cache -eq 'warm') {
                    foreach ($file in (Files $inputPath)) {
                        $reader = [IO.File]::OpenRead($file)
                        try { while ($reader.Read($block,0,$block.Length)) {} } finally { $reader.Dispose() }
                    }
                }
                $watch = [Diagnostics.Stopwatch]::StartNew()
                $child = [Diagnostics.Process]::new()
                $child.StartInfo.FileName = $program
                $child.StartInfo.Arguments = $cli -join ' '
                $child.StartInfo.UseShellExecute = $false
                $child.StartInfo.CreateNoWindow = $true
                $child.StartInfo.RedirectStandardOutput = $true
                $child.StartInfo.RedirectStandardError = $true
                $child.Start() | Out-Null
                $stdout = $child.StandardOutput.ReadToEndAsync()
                $stderr = $child.StandardError.ReadToEndAsync()
                $peak = 0L
                while (-not $child.WaitForExit(15)) {
                    try { $child.Refresh(); $peak = [Math]::Max($peak, $child.PeakWorkingSet64) } catch {}
                }
                $seconds = $watch.Elapsed.TotalSeconds
                $ok = if ($mode.StartsWith('robocopy-')) { $child.ExitCode -lt 8 } else { $child.ExitCode -eq 0 }
                if (-not $ok) { throw "$mode exited $($child.ExitCode). $($stderr.Result) $($stdout.Result)" }
                if ($mode -eq 'experimental-iocp' -and $stdout.Result -notmatch 'IOCP=1') {
                    throw 'The custom IOCP path did not run. Test on two supported physical NTFS SSDs.'
                }
                if ($mode -eq 'copyfile2-unbuffered' -and $stdout.Result -notmatch 'unbuffered=1') {
                    throw 'CopyFile2 did not complete with the unbuffered strategy.'
                }
                $cpu = $child.TotalProcessorTime.TotalSeconds
                $child.Dispose()
                $durable = $null
                if ($scenario.name -eq 'large') {
                    $flush = [Diagnostics.Stopwatch]::StartNew()
                    $f = [IO.File]::OpenWrite($out)
                    try { $f.Flush($true) } finally { $f.Dispose() }
                    $durable = $seconds + $flush.Elapsed.TotalSeconds
                }
                $actualCount=0; $actualBytes=0L
                foreach ($file in (Files $out)) { $actualCount++; $actualBytes += [IO.FileInfo]::new($file).Length }
                if ($actualCount -ne $scenario.count -or $actualBytes -ne $scenario.bytes) { throw "$mode produced incorrect output" }
                [pscustomobject]@{scenario=$scenario.name; mode=$mode; run=$run; cache=$Cache; seconds=$seconds;
                    large_flush_seconds=$durable; files_per_second=$scenario.count/$seconds; MiB_per_second=$scenario.bytes/1MB/$seconds;
                    cpu_seconds=$cpu; observed_peak_working_set_MiB=$peak/1MB} |
                    Export-Csv -NoTypeInformation -Append -Path $Csv
                Write-Host "$($scenario.name) $mode $run/$Runs $([Math]::Round($seconds,2)) s"
                Remove-Fixture $out $dst
            }
        }
    }
} finally {
    Remove-Fixture $src $bin
    Remove-Fixture $dst $dstParent
}
