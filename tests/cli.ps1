param([Parameter(Mandatory)][string]$Exe)
$ErrorActionPreference = 'Stop'
$root = Join-Path (Split-Path -Parent $Exe) ('qcopy-test-' + [guid]::NewGuid())
function Assert($condition, $message) { if (-not $condition) { throw $message } }
function Q([int]$code, [string[]]$arguments) {
    $ErrorActionPreference = 'Continue'
    $output = & $Exe @arguments 2>&1
    Assert ($LASTEXITCODE -eq $code) "Expected $code, got $LASTEXITCODE for $arguments. $output"
}
New-Item -ItemType Directory -Path $root | Out-Null
try {
    $src = [IO.Directory]::CreateDirectory((Join-Path $root 'source')).FullName
    $dst = [IO.Directory]::CreateDirectory((Join-Path $root 'destination')).FullName
    $file = Join-Path $src 'data.bin'
    $target = Join-Path $dst 'data.bin'
    [IO.File]::WriteAllText($file, 'first')
    Q 2 @('copy')
    Q 0 @('copy', '--verify', '--to', $dst, $file)
    Assert ([IO.File]::ReadAllText($target) -eq 'first') 'Incorrect initial copy'
    [IO.File]::WriteAllText($file, 'second')
    Q 1 @('copy', '--to', $dst, $file)
    Assert ([IO.File]::ReadAllText($target) -eq 'first') 'Skip overwrote destination'
    Q 0 @('copy', '--on-conflict', 'rename', '--to', $dst, $file)
    Assert ([IO.File]::ReadAllText((Join-Path $dst 'data (1).bin')) -eq 'second') 'Keep-both failed'
    Q 0 @('copy', '--on-conflict', 'replace', '--to', $dst, $file)
    Assert ([IO.File]::ReadAllText($target) -eq 'second') 'Replacement failed'
    Q 1 @('copy', '--on-conflict', 'replace', '--to', $src, $file)
    Assert ([IO.File]::ReadAllText($file) -eq 'second') 'Self-copy damaged source'
    $tree = [IO.Directory]::CreateDirectory((Join-Path $src 'tree')).FullName
    $empty = [IO.Directory]::CreateDirectory((Join-Path $tree 'empty')).FullName
    [IO.File]::WriteAllBytes((Join-Path $tree 'zero'), [byte[]]@())
    Q 1 @('copy', '--to', $empty, $tree)
    Q 0 @('copy', '--to', $dst, $tree)
    Assert ([IO.Directory]::Exists((Join-Path $dst 'tree\empty'))) 'Empty directory missing'
    Assert ((Get-Item (Join-Path $dst 'tree\zero')).Length -eq 0) 'Zero-byte file missing'
    Q 1 @('move', '--on-conflict', 'replace', '--to', $dst, $tree)
    Q 0 @('move', '--on-conflict', 'rename', '--to', $dst, $tree)
    Assert (-not [IO.Directory]::Exists($tree)) 'Source directory was not moved'
    Assert ([IO.Directory]::Exists((Join-Path $dst 'tree (1)\empty'))) 'Directory rename failed'
    $lock = [IO.File]::Open($target, 'Open', 'ReadWrite', 'None')
    try { Q 1 @('copy', '--on-conflict', 'replace', '--to', $dst, $file) } finally { $lock.Dispose() }
    Assert ([IO.File]::ReadAllText($target) -eq 'second') 'Locked destination was altered'
    Assert (-not @(Get-ChildItem -LiteralPath $dst -Force -Filter '.qcopy-*.part').Count) 'A temporary file leaked'
    Q 0 @('move', '--on-conflict', 'replace', '--to', $dst, $file)
    Assert (-not [IO.File]::Exists($file)) 'Source file was not moved'
    [IO.File]::WriteAllText($file, 'held source')
    $writer = [IO.File]::Open($file, 'Open', 'Write', 'ReadWrite')
    try { Q 1 @('copy', '--on-conflict', 'replace', '--to', $dst, $file) } finally { $writer.Dispose() }
    Assert ([IO.File]::ReadAllText($target) -eq 'second') 'Copy replaced the target while the source was open for writes'
    [IO.File]::Delete($target)
    New-Item -Path $target -ItemType HardLink -Target $file | Out-Null
    Q 1 @('move', '--on-conflict', 'replace', '--to', $dst, $file)
    Assert ([IO.File]::ReadAllText($file) -eq 'held source') 'Hard-link alias move removed the source'
    Assert ([IO.File]::ReadAllText($target) -eq 'held source') 'Hard-link alias was altered'
    $links = [IO.Directory]::CreateDirectory((Join-Path $src 'links')).FullName
    $junction = Join-Path $links 'junction'
    New-Item -Path $junction -ItemType Junction -Target $dst | Out-Null
    try {
        Q 1 @('copy', '--to', $dst, $links)
        Assert (-not [IO.Directory]::Exists((Join-Path $dst 'links\junction'))) 'Copy traversed a junction'
    } finally { [IO.Directory]::Delete($junction) }
    Write-Host 'CLI smoke tests passed'
} finally {
    if ([IO.Path]::GetDirectoryName($root) -eq (Split-Path -Parent $Exe)) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}
