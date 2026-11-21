# Qcopy

[English](README.md) · 简体中文

Qcopy 是 Windows 11 x64 的便携式文件复制和剪切工具。桌面和命令行共用 C++20 / Win32 引擎。发布包有 QcopyDesktop.exe 和 qcopy.exe，普通用户解压后即可运行。

## 下载与使用

从 [GitHub Releases](https://github.com/SilvaNotics/Qcopy/releases/latest) 下载 Windows x64 ZIP 并解压。ZIP 内仅有 `QcopyDesktop.exe` 和 `qcopy.exe`，无需安装。

打开 QcopyDesktop.exe，拖入文件或文件夹，也可以用添加按钮选择。指定现有目标目录，选择「开始复制」或「开始剪切」。默认跳过同名项。覆盖只适用于普通文件。同名目录可跳过或用「保留两份」创建新目录。打开「复制后验证内容」可在发布目标文件前校验 SHA-256。

~~~powershell
.\qcopy.exe copy --to 'D:\备份' 'E:\照片' 'E:\视频.mp4'
.\qcopy.exe move --verify --on-conflict rename --to 'D:\归档' 'E:\待归档'
~~~

桌面可取消任务。命令行用 Ctrl+C 或 Ctrl+Break 取消。退出码 0 表示完成，1 表示有失败或跳过，2 表示参数错误，130 表示取消。命令行被重定向时输出 UTF-8。路径参数以连字符开头时，在源路径前使用双连字符。

## 复制与剪切规则

- 同卷剪切通过文件句柄改名，目录整棵移动。跨卷时逐文件复制，先写目标目录内的临时文件，再发布正式文件名。跨卷剪切在刷盘和发布后删除经确认的源文件。
- 已知的本地内部 SSD 使用最多 4 个递归文件任务。HDD、USB 和未知设备使用 1 个。默认使用系统复制，保持 Windows 的卸载能力。命令行的诊断参数可试跑单线程、系统无缓冲或 IOCP 路径。IOCP 加速只对已识别的独立内部 SSD 上的普通 NTFS 大文件开放试跑，默认关闭。
- 默认省略逐文件的 SHA-256 复读。打开验证时会检查主数据流和命名数据流。跨卷剪切提交后，如果源无法删除，会保留两份并报告原因。
- 递归遇到符号链接、联接点等重解析点时跳过。目标卷支持时，复制会保留文件和目录上的命名流及扩展属性。目标卷无法保存这些数据时，源项会保留。复制新文件继承目标位置的权限。
- 正常取消会清理尚未提交的临时文件。进程被强制终止时，目标目录可能留下名为 .qcopy-*.part 的临时文件。确认任务已停止后，可手动删除这些临时文件。

## 从源码构建

使用 CMake 3.24+、MSVC x64 C++ 工具链与 Windows 11 SDK 10.0.26100.0 或更新版本。通过 x64 Developer PowerShell 运行：

~~~powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix dist
~~~

CTest 包含命令行、Win32 窗口、Unicode 长路径、跨卷文件及故障注入测试。跨卷测试需要系统临时目录与构建目录处于不同卷。测试和跑分辅助程序不打包。只构建发布工具时可用 -DBUILD_TESTING=OFF。

## 性能测试

查看[性能与验收记录](BENCHMARKS.md)。

~~~powershell
.\tests\benchmark.ps1 -Exe .\build\Release\qcopy.exe -Csv .\build\nvme.csv
.\tests\benchmark.ps1 -Exe .\build\Release\qcopy.exe -TargetParent 'D:\Bench' -IncludeExperimentalIocp -Workload large -Csv .\build\dual-ssd.csv
~~~

默认每组跑 5 次，包括约 10 万小文件、混合目录和 32 GiB 单文件。对照 Windows Shell 的 IFileOperation、Robocopy /MT:4、Robocopy /J 以及带相同暂存和发布流程的单线程 CopyFile2。大文件单独记录目标补刷盘后的时间，输出包含吞吐、CPU 时间和采样到的峰值工作集。-Cache warm 在各次之前读取源文件。-Cache cold 要求传入实际的缓存清理回调 -BeforeTrialScript。默认标记为 uncontrolled。结果应按设备组合和缓存条件比较。只有正确性通过且自定义路径的中位吞吐领先单线程基线至少 10% 时，才将其纳入默认策略。
