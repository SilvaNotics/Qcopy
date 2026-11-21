param([Parameter(Mandatory)][string]$Exe, [string]$OtherDirectory = [IO.Path]::GetTempPath())
$ErrorActionPreference = 'Stop'
$sourceParent = [IO.Path]::GetFullPath((Split-Path -Parent $Exe))
$targetParent = [IO.Path]::GetFullPath($OtherDirectory)
if ([IO.Path]::GetPathRoot($sourceParent) -eq [IO.Path]::GetPathRoot($targetParent)) {
    Write-Host 'Skipping: fixtures require different volumes.'
    exit 77
}
$src = Join-Path $sourceParent ('qcopy-vol-' + [guid]::NewGuid())
$dst = Join-Path $targetParent ('qcopy-vol-' + [guid]::NewGuid())
function Assert($condition, $message) { if (-not $condition) { throw $message } }
function Q([int]$code, [string[]]$arguments) {
    $ErrorActionPreference = 'Continue'
    $output = & $Exe @arguments 2>&1
    if ($LASTEXITCODE -ne $code) { throw "Expected $code, got $LASTEXITCODE for $arguments. $output" }
}
function Cleanup([string]$root, [string]$parent) {
    $full = [IO.Path]::GetFullPath($root)
    if ([IO.Path]::GetDirectoryName($full).TrimEnd('\') -ne $parent.TrimEnd('\') -or
        [IO.Path]::GetFileName($full) -notmatch '^qcopy-vol-[a-f0-9-]{36}$') { throw "Invalid fixture path $full" }
    if ([IO.Directory]::Exists($full)) { Remove-Item -LiteralPath $full -Recurse -Force }
}
try {
    [IO.Directory]::CreateDirectory($src) | Out-Null
    [IO.Directory]::CreateDirectory($dst) | Out-Null
    $file = Join-Path $src 'streams.txt'
    $target = Join-Path $dst 'streams.txt'
    [IO.File]::WriteAllText($file, 'main')
    Set-Content -LiteralPath $file -Stream 'metadata' -Value 'named'
    Set-ItemProperty -LiteralPath $file -Name IsReadOnly -Value $true
    Q 0 @('move', '--verify', '--to', $dst, $file)
    Assert (-not [IO.File]::Exists($file)) 'Cross-volume source still exists'
    Assert ([IO.File]::ReadAllText($target) -eq 'main') 'Contents differ'
    Assert ((Get-Content -LiteralPath $target -Stream 'metadata') -eq 'named') 'Named stream was lost'
    Assert ((Get-Item -LiteralPath $target).IsReadOnly) 'Read-only flag was lost'
    $tree = [IO.Directory]::CreateDirectory((Join-Path $src 'tree')).FullName
    $sub = [IO.Directory]::CreateDirectory((Join-Path $tree 'sub')).FullName
    foreach ($i in 0..299) { [IO.File]::WriteAllText((Join-Path $sub "$i.txt"), "value $i") }
    Q 0 @('move', '--to', $dst, $tree)
    Assert (-not [IO.Directory]::Exists($tree)) 'Nonempty directory tree was not removed'
    Assert (@(Get-ChildItem -LiteralPath (Join-Path $dst 'tree\sub')).Count -eq 300) 'Some directory entries were skipped'
    $blocked = Join-Path $src 'blocked'
    [IO.File]::WriteAllText($blocked, 'preserve')
    [IO.File]::WriteAllText((Join-Path $dst 'blocked'), 'original')
    $lock = [IO.File]::Open((Join-Path $dst 'blocked'), 'Open', 'ReadWrite', 'None')
    try { Q 1 @('move', '--on-conflict', 'replace', '--to', $dst, $blocked) } finally { $lock.Dispose() }
    Assert ([IO.File]::ReadAllText($blocked) -eq 'preserve') 'Failed move removed the source'
    Assert ([IO.File]::ReadAllText((Join-Path $dst 'blocked')) -eq 'original') 'Failed move changed the destination'
    Assert (-not @(Get-ChildItem -LiteralPath $dst -Force -Filter '.qcopy-*.part').Count) 'Temporary file leaked'
    $sparse = Join-Path $src 'sparse.bin'
    [IO.File]::WriteAllBytes($sparse, [byte[]]@())
    & fsutil.exe sparse setflag $sparse | Out-Null
    Assert ($LASTEXITCODE -eq 0) 'Setting the sparse flag failed'
    $stream = [IO.File]::OpenWrite($sparse)
    try { $stream.SetLength(4294967419L); $stream.Position = $stream.Length - 1; $stream.WriteByte(63) } finally { $stream.Dispose() }
    Q 0 @('copy', '--to', $dst, $sparse)
    $file = Get-Item (Join-Path $dst 'sparse.bin')
    Assert ($file.Length -eq 4294967419L) '64-bit file size was not preserved'
    Assert (($file.Attributes -band [IO.FileAttributes]::SparseFile) -ne 0) 'The sparse attribute was lost'
    $stream = [IO.File]::OpenRead($file.FullName)
    try { $stream.Position = $stream.Length - 1; Assert ($stream.ReadByte() -eq 63) 'Data after 4 GiB was lost' } finally { $stream.Dispose() }
    Write-Host 'Cross-volume tests passed (ADS, readonly, directory, failed rename, and >4 GiB sparse file)'
} finally {
    Cleanup $src $sourceParent
    Cleanup $dst $targetParent
}
