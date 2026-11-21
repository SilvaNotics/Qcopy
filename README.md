# Qcopy

English · [简体中文](README-zh.md)

Qcopy is a portable file copy and move utility for Windows 11 x64. Its desktop app and command-line tool share a C++20 and Win32 transfer engine.

## Download and run

Download the Windows x64 ZIP from [GitHub Releases](https://github.com/SilvaNotics/Qcopy/releases/latest) and extract it. The ZIP contains two executables and requires no installation.

- **QcopyDesktop.exe** opens a desktop app with a Simplified Chinese interface. Drag in files or folders, or use the add buttons. Select an existing destination folder, then click **开始复制** to copy or **开始剪切** to move. Use **复制后验证内容 (SHA-256)** to verify the copied data before it takes its final name at the destination.
- **qcopy.exe** provides the same copy and move operations from PowerShell, Command Prompt, or a script.

## Command line

```powershell
.\qcopy.exe copy --to 'D:\Backup' 'E:\Photos' 'E:\Video.mp4'
.\qcopy.exe move --verify --on-conflict rename --to 'D:\Archive' 'E:\ToArchive'
```

Use `.\qcopy.exe --help` to see the syntax. The default conflict policy is `skip`. Use `--on-conflict replace` to overwrite an existing regular file or `--on-conflict rename` to keep both items under different names. For conflicting directories, use `skip` or `rename`. Place `--` before the source paths if one begins with a hyphen.

Press Ctrl+C or Ctrl+Break to cancel from the terminal, or use the cancel button in the desktop app. Redirected command-line output is UTF-8.

| Exit code | Meaning |
| --- | --- |
| `0` | Completed successfully |
| `1` | One or more items failed or were skipped |
| `2` | Invalid arguments |
| `130` | Cancelled |

## Transfer behavior

- Same-volume moves use a Windows rename, including for whole directory trees. For cross-volume transfers, Qcopy stages each file in the destination directory before giving it its final name. A cross-volume move flushes and publishes the destination before deleting the confirmed source. If source deletion fails, Qcopy keeps both copies and reports why.
- Optional SHA-256 verification covers both the main data stream and named data streams. Qcopy skips reparse points such as symbolic links and junctions during recursion. It preserves named streams and extended attributes where the target supports them and keeps the source item if the destination cannot store them. Newly copied files inherit permissions from the destination.
- Qcopy uses the Windows copy API by default. Recursive transfers use up to four file tasks on recognized internal SSDs and one on HDDs, USB drives, and unknown devices. The experimental IOCP path is opt-in and limited to eligible large NTFS files across separate recognized internal SSDs.
- Normal cancellation removes uncommitted temporary files. A forced exit can leave `.qcopy-*.part` files in the destination directory. Remove them only after confirming the transfer has stopped.

## Build from source

Install CMake 3.24 or newer, the x64 MSVC C++ toolchain, and Windows 11 SDK 10.0.26100.0 or newer. Run these commands from an x64 Developer PowerShell session.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix dist
```

CTest covers the CLI, desktop window, long Unicode paths, cross-volume transfers, and fault handling. The cross-volume and fault tests require the build directory and system temporary directory to be on different volumes. Use `-DBUILD_TESTING=OFF` to build just the two release executables.

## Benchmarks

The [benchmark and validation notes](BENCHMARKS.md) are in Simplified Chinese. To measure performance on your hardware, run the benchmark script with a Release build.

```powershell
.\tests\benchmark.ps1 -Exe .\build\Release\qcopy.exe -Csv .\build\nvme.csv
.\tests\benchmark.ps1 -Exe .\build\Release\qcopy.exe -TargetParent 'D:\Bench' -IncludeExperimentalIocp -Workload large -Csv .\build\dual-ssd.csv
```
