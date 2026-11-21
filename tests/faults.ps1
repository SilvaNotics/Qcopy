param([Parameter(Mandatory)][string]$Exe, [string]$OtherDirectory = [IO.Path]::GetTempPath())
$ErrorActionPreference = 'Stop'
$sourceParent = [IO.Path]::GetFullPath((Split-Path -Parent $Exe))
$targetParent = [IO.Path]::GetFullPath($OtherDirectory)
if ([IO.Path]::GetPathRoot($sourceParent) -eq [IO.Path]::GetPathRoot($targetParent)) { Write-Host 'Skipping: fixtures require different volumes.'; exit 77 }
$src = Join-Path $sourceParent ('qcopy-fault-' + [guid]::NewGuid())
$dst = Join-Path $targetParent ('qcopy-fault-' + [guid]::NewGuid())
function Assert($condition, $message) { if (-not $condition) { throw $message } }
function Q([int]$code, [string]$mode, [string[]]$arguments) {
    $ErrorActionPreference = 'Continue'
    $env:QCOPY_TEST_FAULT = $mode
    $output = & $Exe @arguments 2>&1
    if ($LASTEXITCODE -ne $code) { throw "Fault $mode expected $code, got $LASTEXITCODE. $output" }
    if ($code -eq 0 -and $arguments -contains 'experimental-iocp') { Assert (($output -join " ") -match 'IOCP=1') 'The pipeline was not exercised' }
}
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class QcopyCtrl {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern bool CreateProcess(string app, System.Text.StringBuilder cmd, IntPtr pa, IntPtr ta,
        bool inherit, uint flags, IntPtr env, string dir, ref SI si, out PI pi);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool GenerateConsoleCtrlEvent(uint code, uint id);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] public static extern uint WaitForSingleObject(IntPtr h, uint ms);
    [DllImport("kernel32.dll")] public static extern bool GetExitCodeProcess(IntPtr h, out uint code);
    [DllImport("kernel32.dll")] public static extern bool TerminateProcess(IntPtr h, uint code);
    [StructLayout(LayoutKind.Sequential)] public struct PI { public IntPtr process, thread; public uint pid, tid; }
    [StructLayout(LayoutKind.Sequential)] public struct SI {
        public uint cb;
        public IntPtr reserved, desktop, title;
        public uint x, y, xSize, ySize, xChars, yChars, fill, flags;
        public ushort show, reservedCount;
        public IntPtr reserved2, stdin, stdout, stderr;
    }
    public static uint Cancel(string app, string command) {
        var si = new SI { cb=(uint)Marshal.SizeOf(typeof(SI)) };
        PI pi;
        if (!CreateProcess(app, new System.Text.StringBuilder(command), IntPtr.Zero, IntPtr.Zero,
             false, 0x200, IntPtr.Zero, null, ref si, out pi)) throw new System.ComponentModel.Win32Exception();
        try {
            System.Threading.Thread.Sleep(100);
            if (!GenerateConsoleCtrlEvent(1, pi.pid)) throw new System.ComponentModel.Win32Exception();
            if (WaitForSingleObject(pi.process, 8000) != 0) throw new TimeoutException();
            uint result;
            if (!GetExitCodeProcess(pi.process, out result)) throw new System.ComponentModel.Win32Exception();
            return result;
        } finally {
            if (WaitForSingleObject(pi.process, 0) == 0x102) TerminateProcess(pi.process, 99);
            CloseHandle(pi.thread);
            CloseHandle(pi.process);
        }
    }
}
'@
try {
    [IO.Directory]::CreateDirectory($src) | Out-Null
    [IO.Directory]::CreateDirectory($dst) | Out-Null
    $file = Join-Path $src 'data.bin'
    $target = Join-Path $dst 'data.bin'
    [IO.File]::WriteAllText($file, 'old source')
    [IO.File]::WriteAllText($target, 'old target')
    $argsMove = @('move', '--on-conflict', 'replace', '--to', $dst, $file)
    foreach ($mode in @('disk-full', 'write-fault')) {
        Q 1 $mode $argsMove
        Assert ([IO.File]::ReadAllText($file) -eq 'old source') "Source changed after $mode"
        Assert ([IO.File]::ReadAllText($target) -eq 'old target') "Destination changed after $mode"
        Assert (-not @(Get-ChildItem -LiteralPath $dst -Force -Filter '.qcopy-*.part').Count) "Partial file leaked after $mode"
    }
    $env:QCOPY_TEST_FAULT = 'wait-ctrl-break'
    $command = '"{0}" move --on-conflict replace --to "{1}" "{2}"' -f $Exe, $dst, $file
    Assert (([QcopyCtrl]::Cancel($Exe, $command)) -eq 130) 'Ctrl+Break did not cancel'
    Assert ([IO.File]::ReadAllText($file) -eq 'old source') 'Cancellation changed the source'
    Assert ([IO.File]::ReadAllText($target) -eq 'old target') 'Cancellation changed the destination'
    Assert (-not @(Get-ChildItem -LiteralPath $dst -Force -Filter '.qcopy-*.part').Count) 'Partial file leaked after cancellation'
    Q 91 'crash-before' $argsMove
    Assert ([IO.File]::ReadAllText($file) -eq 'old source') 'Source changed before commit'
    Assert ([IO.File]::ReadAllText($target) -eq 'old target') 'Destination changed before commit'
    Q 91 'crash-after' $argsMove
    Assert ([IO.File]::ReadAllText($file) -eq 'old source') 'Source changed immediately after commit'
    Assert ([IO.File]::ReadAllText($target) -eq 'old source') 'Destination not published after commit'
    Q 1 'replace-source' $argsMove
    Assert ([IO.File]::ReadAllText($file) -eq 'new source') 'Replacement source deleted'
    Assert ([IO.File]::ReadAllText($file + '.old') -eq 'old source') 'Original source deleted'
    Assert ([IO.File]::ReadAllText($target) -eq 'old source') 'Destination changed after source replacement'
    $large = Join-Path $src 'pipeline.bin'
    $stream = [IO.File]::OpenWrite($large)
    try {
        $stream.SetLength(536870963L)
        $stream.Position = 536870962L
        $stream.WriteByte(97)
    } finally { $stream.Dispose() }
    Q 1 'fail-iocp' @('copy', '--benchmark-strategy', 'experimental-iocp', '--to', $dst, $large)
    Assert (-not [IO.File]::Exists((Join-Path $dst 'pipeline.bin'))) 'Pipeline error published a partial target'
    Assert ([IO.File]::Exists($large)) 'Pipeline error affected the source'
    Q 0 '' @('copy', '--benchmark-strategy', 'experimental-iocp', '--verify', '--to', $dst, $large)
    Assert ((Get-Item (Join-Path $dst 'pipeline.bin')).Length -eq 536870963L) 'Pipeline tail differs'
    Write-Host 'Fault, cancellation, crash, source replacement, and IOCP tail tests passed'
} finally {
    Remove-Item Env:QCOPY_TEST_FAULT -ErrorAction SilentlyContinue
    foreach ($item in @(@($src, $sourceParent), @($dst, $targetParent))) {
        $full = [IO.Path]::GetFullPath($item[0])
        if ([IO.Path]::GetDirectoryName($full).TrimEnd('\') -eq $item[1].TrimEnd('\') -and
            [IO.Path]::GetFileName($full) -match '^qcopy-fault-[a-f0-9-]{36}$' -and [IO.Directory]::Exists($full)) {
            Remove-Item -LiteralPath $full -Recurse -Force
        }
    }
}
