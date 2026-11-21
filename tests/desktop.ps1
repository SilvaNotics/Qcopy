param([Parameter(Mandatory)][string]$Exe)
$ErrorActionPreference = 'Stop'
$parent = [IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($Exe))
$root = Join-Path $parent ('qcopy-ui-' + [guid]::NewGuid())
$process = $null
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class QcopyTestWindow {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    private static extern IntPtr FindWindow(string className, IntPtr windowName);
    public static IntPtr FindQcopy() { return FindWindow("QcopyDesktop.Window", IntPtr.Zero); }
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")]
    public static extern IntPtr GetDlgItem(IntPtr hwnd, int id);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern IntPtr SendMessage(IntPtr hwnd, int msg, IntPtr wParam, string text);
    [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)]
    private static extern IntPtr GetTextMessage(IntPtr hwnd, int msg, IntPtr wParam, System.Text.StringBuilder text);
    public static string ReadText(IntPtr hwnd) {
        var buffer = new System.Text.StringBuilder(4096);
        GetTextMessage(hwnd, 0x000d, new IntPtr(buffer.Capacity), buffer);
        return buffer.ToString();
    }
    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")]
    public static extern bool IsWindowEnabled(IntPtr hwnd);
    [DllImport("kernel32.dll")] private static extern IntPtr GlobalAlloc(uint flags, UIntPtr bytes);
    [DllImport("kernel32.dll")] private static extern IntPtr GlobalLock(IntPtr handle);
    [DllImport("kernel32.dll")] private static extern bool GlobalUnlock(IntPtr handle);
    [DllImport("kernel32.dll")] private static extern IntPtr GlobalFree(IntPtr handle);
    public static void Drop(IntPtr hwnd, string path) {
        byte[] names = System.Text.Encoding.Unicode.GetBytes(path + "\0\0");
        IntPtr drop = GlobalAlloc(0x0042, new UIntPtr((uint)(20 + names.Length)));
        if (drop == IntPtr.Zero) throw new OutOfMemoryException();
        IntPtr data = GlobalLock(drop);
        if (data == IntPtr.Zero) { GlobalFree(drop); throw new OutOfMemoryException(); }
        Marshal.WriteInt32(data, 0, 20);
        Marshal.WriteInt32(data, 16, 1);
        Marshal.Copy(names, 0, IntPtr.Add(data, 20), names.Length);
        GlobalUnlock(drop);
        if (!PostMessage(hwnd, 0x0233, drop, IntPtr.Zero)) {
            GlobalFree(drop);
            throw new InvalidOperationException("Could not post a file drop");
        }
    }
}
'@
function Wait-For([scriptblock]$condition, [string]$message) {
    $clock = [Diagnostics.Stopwatch]::StartNew()
    while ($clock.Elapsed.TotalSeconds -lt 8) {
        if (& $condition) { return }
        Start-Sleep -Milliseconds 40
    }
    throw $message
}
try {
    $src = [IO.Directory]::CreateDirectory((Join-Path $root 'source')).FullName
    $dst = [IO.Directory]::CreateDirectory((Join-Path $root 'target')).FullName
    $file = Join-Path $src 'desktop fixture.txt'
    $target = Join-Path $dst 'desktop fixture.txt'
    [IO.File]::WriteAllText($file, 'desktop copy fixture')
    $process = Start-Process -PassThru -WindowStyle Hidden -FilePath $Exe
    Wait-For {
        if ($process.HasExited) { throw "Desktop exited early with $($process.ExitCode)" }
        $handle = [QcopyTestWindow]::FindQcopy()
        $owner = [uint32]0
        [QcopyTestWindow]::GetWindowThreadProcessId($handle, [ref]$owner) | Out-Null
        $owner -eq $process.Id
    } 'Desktop window was not created'
    $window = [QcopyTestWindow]::FindQcopy()
    $process.Refresh()
    $idle = $process.WorkingSet64
    if ($idle -gt 20MB) { throw "Desktop idle working set exceeded 20 MiB: $idle bytes" }
    $edit = [QcopyTestWindow]::GetDlgItem($window, 104)
    [QcopyTestWindow]::SendMessage($edit, 0x000c, [IntPtr]::Zero, $dst) | Out-Null
    $copy = [QcopyTestWindow]::GetDlgItem($window, 108)
    [QcopyTestWindow]::Drop($window, $file)
    Wait-For { [QcopyTestWindow]::IsWindowEnabled($copy) } 'Dropping the source did not enable the copy button'
    [QcopyTestWindow]::PostMessage($window, 0x0111, [IntPtr]108, $copy) | Out-Null
    Wait-For { [IO.File]::Exists($target) -and [QcopyTestWindow]::IsWindowEnabled($copy) } 'UI copy did not finish'
    if ([IO.File]::ReadAllText($target) -ne 'desktop copy fixture') { throw 'Desktop copy wrote incorrect data' }
    [QcopyTestWindow]::PostMessage($window, 0x0111, [IntPtr]108, $copy) | Out-Null
    $log = [QcopyTestWindow]::GetDlgItem($window, 111)
    Wait-For { [QcopyTestWindow]::ReadText($log) -match 'Name conflict' } 'The existing-file skip was not shown in the details panel'
    [IO.File]::Delete($target)
    $move = [QcopyTestWindow]::GetDlgItem($window, 109)
    [QcopyTestWindow]::PostMessage($window, 0x0111, [IntPtr]109, $move) | Out-Null
    Wait-For { [IO.File]::Exists($target) -and -not [IO.File]::Exists($file) -and [QcopyTestWindow]::IsWindowEnabled($move) } 'UI move did not finish'
    [QcopyTestWindow]::PostMessage($window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    if (-not $process.WaitForExit(8000)) { throw 'Desktop did not shut down' }
    Write-Host ('Desktop window drag-and-drop, copy, and move passed. Idle total working set {0:N2} MiB.' -f ($idle / 1MB))
} finally {
    if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id }
    if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($root)) -eq $parent -and
        [IO.Path]::GetFileName($root) -match '^qcopy-ui-[a-f0-9-]{36}$' -and [IO.Directory]::Exists($root)) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}
